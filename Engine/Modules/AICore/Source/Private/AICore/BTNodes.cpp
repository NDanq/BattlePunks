#include <algorithm>
#include <cmath>
#include <cstring>

#include "Private/AICore/BTInternal.h"
#include "Private/AICore/NavInternal.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/BehaviorTree.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/*
 * The built-in behavior tree nodes. Each is stateless: what it keeps is in
 * its BTNodeMemory (Time, Vector, Values, Counter).
 *
 * TASKS
 *   Wait            Seconds, RandomDeviation
 *   SetBlackboard   Key, Value (text, parsed by the key's type) or From (a key to copy)
 *   ClearBlackboard Key
 *   MoveTo          Key (an Entity or Vector) or X/Y/Z; AcceptanceRadius;
 *                   RepathDistance (a moving target re-requested past it, 100)
 *   MoveToRandom    Radius (1000); Origin (a key; else where it stands);
 *                   Out (a Vector key told the point)
 *   FindCover       Threat (a key), Out (a Vector key); Radius (1200),
 *                   Samples (14), MinDistance (400): the nearest hidden point
 *   FaceTarget      Key; Tolerance (degrees, 10); Keep (go on facing after)
 *   PlayAnimParam   Param, Value -- through the AIWorld's BTHost
 *   RunScript       Method, Arg (an Entity key) -- the entity's script
 *                   method; it returns true to succeed
 *   Log             Text
 *
 * DECORATORS
 *   BlackboardCondition  Key, Op (IsSet, IsNotSet, Equals, NotEquals, Less,
 *                        LessOrEqual, Greater, GreaterOrEqual), Value
 *   Cooldown             Seconds: not again until that long after it last ended
 *   TimeLimit            Seconds: aborted (Failure) if it runs longer
 *   Loop                 Count (0 forever): again on Success; a Failure ends it
 *   Inverter, ForceSuccess, ForceFailure
 *
 * SERVICES
 *   DistanceToKey   Key (Entity or Vector), Out (a Float key)
 */
namespace AICore
{
    namespace
    {
        NavAgentComponent* AgentOf(BTContext& InContext)
        {
            return InContext.World.GetEntity(InContext.Self).GetComponent<NavAgentComponent>();
        }

        NavWorld* NavOf(BTContext& InContext)
        {
            return InContext.AI ? &InContext.AI->GetNav() : nullptr;
        }

        /* Where a MoveTo or FaceTarget goes: its Key, or X/Y/Z. */
        bool TargetOf(BTContext& InContext, EngineCore::Vector3& OutTarget)
        {
            const int32_t Key = InContext.GetKey("Key");
            if(Key >= 0) { return BTDetail::ResolveKeyPosition(InContext.Instance, Key, OutTarget); }
            if(InContext.FindParam("X") && InContext.FindParam("Z"))
            {
                OutTarget = EngineCore::Vector3(InContext.GetNumber("X"), InContext.GetNumber("Y"), InContext.GetNumber("Z"));
                return true;
            }
            return false;
        }

        float YawTo(const EngineCore::Vector3& InFrom, const EngineCore::Vector3& InTo)
        {
            return float(std::atan2(double(InTo.GetX()) - InFrom.GetX(), double(InTo.GetZ()) - InFrom.GetZ()) * 180.0 / 3.14159265358979323846);
        }

        float WrapDegrees(float InDegrees)
        {
            while(InDegrees > 180.0f)  { InDegrees -= 360.0f; }
            while(InDegrees < -180.0f) { InDegrees += 360.0f; }
            return InDegrees;
        }

        // === Moving =============================================================

        /* What MoveTo and MoveToRandom share once they have a point:
         * Values[0] the agent's own acceptance radius, to put back;
         * Flags bit 1: it overrode it. */
        EBTStatus StartMove(BTContext& InContext, const EngineCore::Vector3& InTarget)
        {
            NavAgentComponent* Agent = AgentOf(InContext);
            NavWorld* Nav = NavOf(InContext);
            if(!Agent || !Nav) { return EBTStatus::Failure; }
            if(InContext.FindParam("AcceptanceRadius"))
            {
                InContext.Memory.Values[0] = Agent->AcceptanceRadius;
                InContext.Memory.Flags |= 2;
                Agent->AcceptanceRadius = float(InContext.GetNumber("AcceptanceRadius", Agent->AcceptanceRadius));
            }
            const EngineCore::Vector3 Here = BTDetail::SelfPosition(InContext.Instance);
            if(NavDetail::Distance2D(Here, InTarget) <= Agent->AcceptanceRadius) { return EBTStatus::Success; }
            InContext.Memory.Vector = InTarget;
            if(!Nav->MoveTo(InContext.Self, InTarget)) { return EBTStatus::Failure; }
            return EBTStatus::Running;
        }

        EBTStatus FollowMove(BTContext& InContext)
        {
            NavAgentComponent* Agent = AgentOf(InContext);
            if(!Agent) { return EBTStatus::Failure; }
            switch(Agent->Status)
            {
                case NAS_Arrived: return EBTStatus::Success;
                case NAS_Failed:  return EBTStatus::Failure;
                case NAS_Idle:    return Agent->bHasTarget ? EBTStatus::Running : EBTStatus::Failure;
                default:          return EBTStatus::Running;
            }
        }

        void EndMove(BTContext& InContext, const EBTStatus InStatus)
        {
            NavAgentComponent* Agent = AgentOf(InContext);
            if(!Agent) { return; }
            if(InStatus == EBTStatus::Aborted || Agent->bHasTarget)
            {
                if(NavWorld* Nav = NavOf(InContext)) { Nav->StopMoving(InContext.Self); }
            }
            if(InContext.Memory.Flags & 2)
            {
                Agent->AcceptanceRadius = InContext.Memory.Values[0];
                InContext.Memory.Flags &= uint8_t(~2);
            }
        }
    }

    class Wait : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            const double Seconds   = InContext.GetNumber("Seconds", 1.0);
            const double Deviation = InContext.GetNumber("RandomDeviation", 0.0);
            InContext.Memory.Time = InContext.Time + std::max(0.0, Seconds + (double(InContext.Random()) * 2.0 - 1.0) * Deviation);
            return EBTStatus::Running;
        }
        EBTStatus OnTick(BTContext& InContext) const override
        {
            return InContext.Time >= InContext.Memory.Time ? EBTStatus::Success : EBTStatus::Running;
        }
    };
    AICORE_DEFINE_BT_TASK(Wait, "Wait")

    class SetBlackboard : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            const int32_t Key = InContext.GetKey("Key");
            if(Key < 0) { return EBTStatus::Failure; }
            const int32_t From = InContext.GetKey("From");
            if(From >= 0)
            {
                InContext.Board.SetValue(Key, InContext.Board.GetValue(From));
                return EBTStatus::Success;
            }
            const BTParam* Value = InContext.FindParam("Value");
            if(!Value) { return EBTStatus::Failure; }
            InContext.Board.SetName(Key, Value->Text);
            return EBTStatus::Success;
        }
    };
    AICORE_DEFINE_BT_TASK(SetBlackboard, "Set Blackboard")

    class ClearBlackboard : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            const int32_t Key = InContext.GetKey("Key");
            if(Key < 0) { return EBTStatus::Failure; }
            InContext.Board.Clear(Key);
            return EBTStatus::Success;
        }
    };
    AICORE_DEFINE_BT_TASK(ClearBlackboard, "Clear Blackboard")

    class MoveTo : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            EngineCore::Vector3 Target;
            if(!TargetOf(InContext, Target)) { return EBTStatus::Failure; }
            return StartMove(InContext, Target);
        }
        EBTStatus OnTick(BTContext& InContext) const override
        {
            /* A target that moves: follow it once it has gone far enough. */
            const int32_t Key = InContext.GetKey("Key");
            if(Key >= 0)
            {
                EngineCore::Vector3 Target;
                if(!BTDetail::ResolveKeyPosition(InContext.Instance, Key, Target)) { return EBTStatus::Failure; }
                if(NavDetail::Distance2D(Target, InContext.Memory.Vector) > float(InContext.GetNumber("RepathDistance", 100.0)))
                {
                    InContext.Memory.Vector = Target;
                    NavWorld* Nav = NavOf(InContext);
                    if(!Nav || !Nav->MoveTo(InContext.Self, Target)) { return EBTStatus::Failure; }
                }
            }
            return FollowMove(InContext);
        }
        void OnExit(BTContext& InContext, const EBTStatus InStatus) const override { EndMove(InContext, InStatus); }
    };
    AICORE_DEFINE_BT_TASK(MoveTo, "Move To")

    class MoveToRandom : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            NavWorld* Nav = NavOf(InContext);
            if(!Nav || !Nav->HasNavMesh()) { return EBTStatus::Failure; }
            EngineCore::Vector3 Origin = BTDetail::SelfPosition(InContext.Instance);
            const int32_t OriginKey = InContext.GetKey("Origin");
            if(OriginKey >= 0) { BTDetail::ResolveKeyPosition(InContext.Instance, OriginKey, Origin); }
            const float Radius = float(InContext.GetNumber("Radius", 1000.0));
            const uint32_t Seed = uint32_t(InContext.Random() * 4294967040.0f) | 1u;
            EngineCore::Vector3 Point;
            if(!Nav->GetQuery().RandomPointInRadius(Origin, Radius, Seed, Point, &Nav->GetDefaultFilter())) { return EBTStatus::Failure; }
            const int32_t Out = InContext.GetKey("Out");
            if(Out >= 0) { InContext.Board.SetVector(Out, Point); }
            return StartMove(InContext, Point);
        }
        EBTStatus OnTick(BTContext& InContext) const override { return FollowMove(InContext); }
        void OnExit(BTContext& InContext, const EBTStatus InStatus) const override { EndMove(InContext, InStatus); }
    };
    AICORE_DEFINE_BT_TASK(MoveToRandom, "Move To Random Point")

    /* A navmesh point near the brain that its Threat cannot see: up to Samples
     * random points within Radius, the nearest one hidden (the host's line of
     * sight, from the threat's eye to the point's chest) and at least
     * MinDistance from the threat. Out is told it. Failure when none hides. */
    class FindCover : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            NavWorld* Nav = NavOf(InContext);
            const int32_t Out = InContext.GetKey("Out");
            if(!Nav || !Nav->HasNavMesh() || Out < 0) { return EBTStatus::Failure; }
            EngineCore::Vector3 Threat;
            if(!BTDetail::ResolveKeyPosition(InContext.Instance, InContext.GetKey("Threat"), Threat)) { return EBTStatus::Failure; }
            const AIHost& Host = InContext.AI->GetPerception().GetHost();
            const EngineCore::Vector3 Here = BTDetail::SelfPosition(InContext.Instance);
            const EngineCore::Vector3 Eye(Threat.GetX(), Threat.GetY() + float(InContext.GetNumber("ThreatHeight", 150.0)), Threat.GetZ());
            const float Radius   = float(InContext.GetNumber("Radius", 1200.0));
            const float Away     = float(InContext.GetNumber("MinDistance", 400.0));
            const int   Samples  = std::clamp(int(InContext.GetNumber("Samples", 14.0)), 1, 64);

            GameCore::EntityID ThreatEntity;
            if(const int32_t ThreatKey = InContext.GetKey("Threat"); ThreatKey >= 0) { ThreatEntity = InContext.Board.GetValue(ThreatKey).EntityValue; }

            bool bFound = false;
            float Best = 1e30f;
            EngineCore::Vector3 Chosen;
            for(int Sample = 0; Sample < Samples; ++Sample)
            {
                EngineCore::Vector3 Point;
                const uint32_t Seed = uint32_t(InContext.Random() * 4294967040.0f) | 1u;
                if(!Nav->GetQuery().RandomPointInRadius(Here, Radius, Seed, Point, &Nav->GetDefaultFilter())) { continue; }
                if(NavDetail::Distance2D(Point, Threat) < Away) { continue; }
                const EngineCore::Vector3 Chest(Point.GetX(), Point.GetY() + 110.0f, Point.GetZ());
                /* Hidden: the threat's line of sight is cut (no host: nothing hides). */
                if(!Host.LineOfSight || Host.LineOfSight(InContext.World, Eye, Chest, ThreatEntity, InContext.Self, Host.UserData)) { continue; }
                const float D = NavDetail::Distance2D(Point, Here);
                if(D < Best) { Best = D; Chosen = Point; bFound = true; }
            }
            if(!bFound) { return EBTStatus::Failure; }
            InContext.Board.SetVector(Out, Chosen);
            return EBTStatus::Success;
        }
    };
    AICORE_DEFINE_BT_TASK(FindCover, "Find Cover")

    class FaceTarget : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            if(!AgentOf(InContext)) { return EBTStatus::Failure; }
            InContext.Memory.Time = InContext.Time + InContext.GetNumber("TimeLimit", 3.0);
            return Update(InContext);
        }
        EBTStatus OnTick(BTContext& InContext) const override
        {
            if(InContext.Time >= InContext.Memory.Time) { return EBTStatus::Failure; }
            return Update(InContext);
        }
        void OnExit(BTContext& InContext, const EBTStatus InStatus) const override
        {
            (void)InStatus;
            NavAgentComponent* Agent = AgentOf(InContext);
            if(Agent && !InContext.GetBool("Keep", false)) { Agent->bHasFacing = 0; }
        }

    private:
        static EBTStatus Update(BTContext& InContext)
        {
            NavAgentComponent* Agent = AgentOf(InContext);
            EngineCore::Vector3 Target;
            if(!Agent || !TargetOf(InContext, Target)) { return EBTStatus::Failure; }
            Agent->FaceTarget = Target;
            Agent->bHasFacing = 1;
            const GameCore::TransformPose Pose = InContext.World.GetEntity(InContext.Self).GetWorldPose();
            const float Error = std::fabs(WrapDegrees(YawTo(Pose.Position, Target) - GetFacingYaw(InContext.World.GetEntity(InContext.Self))));
            return Error <= float(InContext.GetNumber("Tolerance", 10.0)) ? EBTStatus::Success : EBTStatus::Running;
        }
    };
    AICORE_DEFINE_BT_TASK(FaceTarget, "Face Target")

    class PlayAnimParam : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            if(!InContext.AI) { return EBTStatus::Failure; }
            const BTHost& Host = InContext.AI->GetTreeHost();
            if(!Host.SetAnimParam) { return EBTStatus::Failure; }
            const bool bDone = Host.SetAnimParam(InContext.World, InContext.Self, InContext.GetString("Param"),
                                                 float(InContext.GetNumber("Value", 0.0)), Host.UserData);
            return bDone ? EBTStatus::Success : EBTStatus::Failure;
        }
    };
    AICORE_DEFINE_BT_TASK(PlayAnimParam, "Play Anim Param")

    class RunScript : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            const char* Method = InContext.GetString("Method");
            if(!Method[0]) { return EBTStatus::Failure; }
            GameCore::EntityID Arg;
            const int32_t Key = InContext.GetKey("Arg");
            if(Key >= 0) { Arg = InContext.Board.GetValue(Key).EntityValue; }
            return InContext.World.CallScriptMethod(InContext.Self, Method, Arg) ? EBTStatus::Success : EBTStatus::Failure;
        }
    };
    AICORE_DEFINE_BT_TASK(RunScript, "Run Script")

    class Log : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            CORE_LOG(std::string("[BT] ") + InContext.Instance.GetAsset().Name + ": " + InContext.GetString("Text"))
            return EBTStatus::Success;
        }
    };
    AICORE_DEFINE_BT_TASK(Log, "Log")

    // === Decorators ===========================================================

    class BlackboardCondition : public BTDecorator
    {
    public:
        bool IsCondition() const override { return true; }
        bool ObservesBlackboard() const override { return true; }
        bool Evaluate(BTContext& InContext) const override
        {
            const int32_t Key = InContext.GetKey("Key");
            if(Key < 0) { return false; }
            const char* Op = InContext.GetString("Op", "IsSet");
            const BlackboardValue& Value = InContext.Board.GetValue(Key);
            if(std::strcmp(Op, "IsSet") == 0)    { return IsTrue(InContext.Board, Key, Value); }
            if(std::strcmp(Op, "IsNotSet") == 0) { return !IsTrue(InContext.Board, Key, Value); }

            const BTParam* Operand = InContext.FindParam("Value");
            if(!Operand) { return false; }
            double Right = Operand->Number;
            if(!Operand->bIsNumber && Operand->Key >= 0) { Right = InContext.Board.GetValue(Operand->Key).AsNumber(); }
            if(std::strcmp(Op, "Equals") == 0 || std::strcmp(Op, "NotEquals") == 0)
            {
                bool bEqual;
                if(Value.Type == EBlackboardType::Name) { bEqual = Value.NameValue == Operand->Text; }
                else
                {
                    BlackboardValue Parsed = Value;
                    bEqual = (Operand->bIsNumber || Parsed.Parse(Operand->Text))
                        ? std::fabs(Value.AsNumber() - (Operand->bIsNumber ? Right : Parsed.AsNumber())) < 1e-6
                        : false;
                }
                return (std::strcmp(Op, "Equals") == 0) == bEqual;
            }
            const double Left = Value.AsNumber();
            if(std::strcmp(Op, "Less") == 0)           { return Left < Right; }
            if(std::strcmp(Op, "LessOrEqual") == 0)    { return Left <= Right; }
            if(std::strcmp(Op, "Greater") == 0)        { return Left > Right; }
            if(std::strcmp(Op, "GreaterOrEqual") == 0) { return Left >= Right; }
            return false;
        }

    private:
        /* Set, and for a bool or a number, true. */
        static bool IsTrue(const Blackboard& InBoard, const int32_t InKey, const BlackboardValue& InValue)
        {
            if(!InBoard.IsSet(InKey)) { return false; }
            if(InValue.Type == EBlackboardType::Bool) { return InValue.bValue; }
            return true;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(BlackboardCondition)

    class Cooldown : public BTDecorator
    {
    public:
        bool IsCondition() const override { return true; }
        bool Evaluate(BTContext& InContext) const override
        {
            return (InContext.Memory.Flags & 1) == 0 || InContext.Time >= InContext.Memory.Time;
        }
        EBTStatus OnResult(BTContext& InContext, const EBTStatus InStatus) const override
        {
            InContext.Memory.Flags |= 1;
            InContext.Memory.Time = InContext.Time + InContext.GetNumber("Seconds", 1.0);
            return InStatus;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(Cooldown)

    class TimeLimit : public BTDecorator
    {
    public:
        void OnEnter(BTContext& InContext) const override
        {
            InContext.Memory.Time = InContext.Time + InContext.GetNumber("Seconds", 5.0);
        }
        bool KeepRunning(BTContext& InContext) const override { return InContext.Time < InContext.Memory.Time; }
    };
    AICORE_DEFINE_BT_DECORATOR(TimeLimit)

    class Loop : public BTDecorator
    {
    public:
        EBTStatus OnResult(BTContext& InContext, const EBTStatus InStatus) const override
        {
            if(InStatus != EBTStatus::Success) { InContext.Memory.Counter = 0; return InStatus; }
            ++InContext.Memory.Counter;
            const int32_t Count = int32_t(InContext.GetNumber("Count", 0.0));
            if(Count <= 0 || InContext.Memory.Counter < Count) { return EBTStatus::Running; }
            InContext.Memory.Counter = 0;
            return EBTStatus::Success;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(Loop)

    class Inverter : public BTDecorator
    {
    public:
        EBTStatus OnResult(BTContext& InContext, const EBTStatus InStatus) const override
        {
            (void)InContext;
            if(InStatus == EBTStatus::Success) { return EBTStatus::Failure; }
            if(InStatus == EBTStatus::Failure) { return EBTStatus::Success; }
            return InStatus;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(Inverter)

    class ForceSuccess : public BTDecorator
    {
    public:
        EBTStatus OnResult(BTContext& InContext, const EBTStatus InStatus) const override
        {
            (void)InContext;
            return InStatus == EBTStatus::Failure ? EBTStatus::Success : InStatus;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(ForceSuccess)

    class ForceFailure : public BTDecorator
    {
    public:
        EBTStatus OnResult(BTContext& InContext, const EBTStatus InStatus) const override
        {
            (void)InContext;
            return InStatus == EBTStatus::Success ? EBTStatus::Failure : InStatus;
        }
    };
    AICORE_DEFINE_BT_DECORATOR(ForceFailure)

    // === Services =============================================================

    class DistanceToKey : public BTService
    {
    public:
        void OnTick(BTContext& InContext) const override
        {
            const int32_t Out = InContext.GetKey("Out");
            if(Out < 0) { return; }
            EngineCore::Vector3 Where;
            if(!BTDetail::ResolveKeyPosition(InContext.Instance, InContext.GetKey("Key"), Where))
            {
                InContext.Board.Clear(Out);
                return;
            }
            InContext.Board.SetFloat(Out, NavDetail::Distance3D(BTDetail::SelfPosition(InContext.Instance), Where));
        }
    };
    AICORE_DEFINE_BT_SERVICE(DistanceToKey)
}
