#include "Public/NimbleEngine/AIFacade.h"
#include "Private/NimbleEngine/AIInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/Components/AnimationComponent.h"

#if WITH_AICORE
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/NavDebugDraw.h"
#endif

/* AIFacade.h has the contract. */

namespace
{
    NIMBLE_CONFIG_BOOL(GAIShowNavMesh, "AI", "ShowNavMesh", false, nullptr);
    NIMBLE_CONFIG_BOOL(GAIShowPaths,   "AI", "ShowPaths",   false, nullptr);
}

namespace NimbleEngine
{
    bool IsAIAvailable() { return WITH_AICORE != 0; }

#if WITH_AICORE
    namespace
    {
        constexpr double DEGREES = 180.0 / 3.14159265358979323846;

        double WrapDegrees(double InDegrees)
        {
            while(InDegrees > 180.0)  { InDegrees -= 360.0; }
            while(InDegrees < -180.0) { InDegrees += 360.0; }
            return InDegrees;
        }

        /* The topmost parent: a prefab's parts are one actor. */
        GameCore::EntityID RootOf(GameCore::World& InWorld, GameCore::EntityID InEntity)
        {
            for(int Depth = 0; Depth < 32 && InWorld.IsAlive(InEntity); ++Depth)
            {
                const GameCore::EntityID Parent = InWorld.GetEntity(InEntity).GetParent();
                if(!Parent.IsSet()) { break; }
                InEntity = Parent;
            }
            return InEntity;
        }

        /* Physics between eye and chest: the viewer's own colliders are
         * stepped past, the target's (its capsule, its hit boxes) count as
         * seeing it. */
        bool LineOfSight(GameCore::World& InWorld, const EngineCore::Vector3& InFrom, const EngineCore::Vector3& InTo,
                         const GameCore::EntityID InViewer, const GameCore::EntityID InTarget, void*)
        {
            const GameCore::EntityID ViewerRoot = RootOf(InWorld, InViewer);
            const GameCore::EntityID TargetRoot = RootOf(InWorld, InTarget);
            EngineCore::Vector3 From = InFrom;
            for(int Step = 0; Step < 4; ++Step)
            {
                EngineCore::Vector3 Delta = InTo - From;
                const float Length = float(Delta.GetLength());
                if(Length < 1.0f) { return true; }
                const EngineCore::Vector3 Direction = Delta / Length;
                PhysicsRaycastHit Hit;
                if(!PhysicsRaycast(InWorld, From, Direction, Length, Hit)) { return true; }
                if(Hit.Distance >= Length - 20.0f) { return true; }
                const GameCore::EntityID HitRoot = RootOf(InWorld, Hit.Entity);
                if(HitRoot == TargetRoot) { return true; }
                if(HitRoot != ViewerRoot) { return false; }
                From = Hit.Point + Direction * 2.0f;
            }
            return false;
        }

        bool GroundHeight(GameCore::World& InWorld, const EngineCore::Vector3& InAt, float& OutHeight, void*)
        {
            PhysicsRaycastHit Hit;
            if(!PhysicsRaycast(InWorld, InAt, EngineCore::Vector3(0.0, -1.0, 0.0), 400.0f, Hit)) { return false; }
            OutHeight = float(Hit.Point.GetY());
            return true;
        }

        bool SetAnimParam(GameCore::World& InWorld, const GameCore::EntityID InEntity, const char* InParam, const float InValue, void*)
        {
            RenderCore::AnimationComponent* Animation = InWorld.GetEntity(InEntity).GetComponent<RenderCore::AnimationComponent>();
            return Animation && Animation->SetFloat(InParam, InValue);
        }

        /* Attached for the call, if the World has none (a tool's bake). */
        struct ScopedAI
        {
            GameCore::World& World;
            bool bAttached = false;
            AICore::AIWorld* AI = nullptr;
            explicit ScopedAI(GameCore::World& InWorld) : World(InWorld)
            {
                AI = AICore::AIWorld::Find(&InWorld);
                if(!AI) { AI = AICore::AIWorld::Attach(InWorld); bAttached = true; }
            }
            ~ScopedAI() { if(bAttached) { AICore::AIWorld::Detach(World); } }
        };
    }

    namespace AIDetail
    {
        void AttachLevelAI(GameCore::World& InWorld)
        {
            /* A client's avatars and NPCs follow the server's. */
            if(IsClient()) { return; }
            AICore::AIWorld* AI = AICore::AIWorld::Attach(InWorld);
            AICore::AIHost Host;
            Host.LineOfSight  = &LineOfSight;
            Host.GroundHeight = &GroundHeight;
            AI->SetHost(Host);
            AICore::BTHost TreeHost;
            TreeHost.SetAnimParam = &SetAnimParam;
            AI->SetTreeHost(TreeHost);
        }

        void DetachLevelAI(GameCore::World& InWorld)
        {
            if(AICore::AIWorld::Find(&InWorld)) { AICore::AIWorld::Detach(InWorld); }
        }

        void DrawAIDebug(GameCore::World& InWorld, RenderCore::Renderer& InRenderer)
        {
#if WITH_EDITOR
            const bool bMesh = GAIShowNavMesh.GetBool(), bPaths = GAIShowPaths.GetBool();
            if(!bMesh && !bPaths) { return; }
            AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
            if(!AI || !AI->GetNav().HasNavMesh()) { return; }

            std::vector<AICore::NavDebugLine> Lines;
            if(bMesh) { AICore::DrawNavMesh(AI->GetNav().GetNavMesh(), Lines, AICore::NDF_Default | AICore::NDF_Obstacles); }
            if(bPaths)
            {
                std::vector<EngineCore::Vector3> Corners;
                InWorld.Each<AICore::NavAgentComponent>([&](const GameCore::Entity& InEntity, AICore::NavAgentComponent&)
                {
                    if(AI->GetNav().GetAgentCorners(InEntity.GetID(), Corners) > 1) { AICore::DrawNavPath(Corners, Lines); }
                });
            }
            for(const AICore::NavDebugLine& Line : Lines)
            {
                RenderCore::DebugLine Out;
                Out.Start = Line.From;
                Out.End   = Line.To;
                Out.Color = EngineCore::Color(uint8_t((Line.Color >> 16) & 0xFF), uint8_t((Line.Color >> 8) & 0xFF), uint8_t(Line.Color & 0xFF),
                                              uint8_t((Line.Color >> 24) & 0xFF));
                Out.ThicknessPixels = 1.5f;
                InRenderer.DrawDebugLine(Out);
            }
#else
            (void)InWorld;
            (void)InRenderer;
#endif
        }
    }

    float SteerInput(GameCore::World& InWorld, const GameCore::EntityID InAvatar, const SteerInputMap& InMap, const float InYaw,
                     const float InDeltaSeconds, InputSnapshot& InOutInput)
    {
        const AICore::NavAgentComponent* Agent = InWorld.GetEntity(InAvatar).GetComponent<AICore::NavAgentComponent>();
        if(!Agent) { return 0.0f; }
        const EngineCore::Vector3 Here = InWorld.GetEntity(InAvatar).GetWorldPose().Position;
        const double VX = Agent->DesiredVelocity.GetX(), VZ = Agent->DesiredVelocity.GetZ();
        const double Speed = std::sqrt(VX * VX + VZ * VZ);

        /* Where to look: the facing target, else the way it goes. */
        double WantYaw = double(InYaw);
        if(Agent->bHasFacing)
        {
            const double DX = double(Agent->FaceTarget.GetX()) - Here.GetX(), DZ = double(Agent->FaceTarget.GetZ()) - Here.GetZ();
            if(DX * DX + DZ * DZ > 1.0) { WantYaw = std::atan2(DX, DZ) * DEGREES; }
        }
        else if(Speed > 10.0)
        {
            WantYaw = std::atan2(VX, VZ) * DEGREES;
        }
        const double Error = WrapDegrees(WantYaw - double(InYaw));
        if(InMap.LookX >= 0)
        {
            const double Most = std::max(double(InMap.TurnDegreesPerSecond) * std::max(double(InDeltaSeconds), 1.0 / 240.0), 0.05);
            InOutInput.Axes[InMap.LookX] = float(std::clamp(Error, -Most, Most));
        }

        /* Moving: the wish direction in the avatar's own frame (the inverse
         * of the walkers' WishX = sin F + cos R, WishZ = cos F - sin R). */
        if(Speed > 10.0)
        {
            const double Yaw = double(InYaw) / DEGREES;
            const double S = std::sin(Yaw), C = std::cos(Yaw);
            const double DirX = VX / Speed, DirZ = VZ / Speed;
            double Forward = S * DirX + C * DirZ;
            double Right   = C * DirX - S * DirZ;
            /* Facing well away from where it must go (it turns first when it
             * has no facing target to keep). */
            if(!Agent->bHasFacing && std::fabs(Error) > double(InMap.FaceMoveTolerance)) { Forward = Right = 0.0; }
            const double Scale = std::clamp(Speed / std::max(double(InMap.WalkSpeed), 1.0), 0.0, 1.0);
            if(InMap.MoveForward >= 0) { InOutInput.Axes[InMap.MoveForward] = float(std::clamp(Forward * Scale, -1.0, 1.0)); }
            if(InMap.MoveRight >= 0)   { InOutInput.Axes[InMap.MoveRight]   = float(std::clamp(Right * Scale, -1.0, 1.0)); }
            if(Speed > double(InMap.RunSpeed)) { InOutInput.Down |= InMap.RunBits; }
        }
        return float(Error);
    }

    bool MoveAIAgent(GameCore::World& InWorld, const GameCore::EntityID InAgent, const EngineCore::Vector3& InTarget)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        return AI && AI->GetNav().MoveTo(InAgent, InTarget);
    }

    void StopAIAgent(GameCore::World& InWorld, const GameCore::EntityID InAgent)
    {
        if(AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld)) { AI->GetNav().StopMoving(InAgent); }
    }

    void ReportAINoise(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, const float InLoudness, const GameCore::EntityID InInstigator)
    {
        AICore::ReportNoise(InWorld, InPosition, InLoudness, InInstigator);
    }

    bool RandomNavPoint(GameCore::World& InWorld, const EngineCore::Vector3& InCenter, const float InRadius, EngineCore::Vector3& OutPoint)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        if(!AI || !AI->GetNav().EnsureNavMesh()) { return false; }
        static uint32_t Seed = 0x9E3779B9u;
        Seed = Seed * 1664525u + 1013904223u;
        return AI->GetNav().GetQuery().RandomPointInRadius(InCenter, InRadius, Seed | 1u, OutPoint, &AI->GetNav().GetDefaultFilter());
    }

    float NavPathLength(GameCore::World& InWorld, const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        if(!AI || !AI->GetNav().EnsureNavMesh()) { return -1.0f; }
        std::vector<EngineCore::Vector3> Points;
        if(AI->GetNav().GetQuery().FindPath(InStart, InEnd, Points, &AI->GetNav().GetDefaultFilter()) != AICore::ENavPathStatus::Complete)
        {
            return -1.0f;
        }
        return AICore::NavQuery::GetPathLength(Points);
    }

    bool BuildNavigation(GameCore::World& InWorld, const char* InDiskPath, std::string* OutReport)
    {
        ScopedAI Scope(InWorld);
        AICore::NavBuildStats Stats;
        std::string Error;
        if(!Scope.AI->GetNav().Build(&Stats, &Error))
        {
            if(OutReport) { *OutReport = Error; }
            return false;
        }
        char Report[160];
        std::snprintf(Report, sizeof(Report), "%d tiles, %d polygons from %d triangles in %.0f ms (%.1f KB)", Stats.Tiles, Stats.Polygons,
                      Stats.Triangles, Stats.Milliseconds, double(Stats.Bytes) / 1024.0);
        if(OutReport) { *OutReport = Report; }
        if(InDiskPath && InDiskPath[0] && !Scope.AI->GetNav().SaveFile(InDiskPath, &Error))
        {
            if(OutReport) { *OutReport = std::string(InDiskPath) + " " + Error; }
            return false;
        }
        return true;
    }

    double GetAIKeyNumber(GameCore::World& InWorld, const GameCore::EntityID InEntity, const char* InKey, const double InDefault)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(InEntity) : nullptr;
        return Board && Board->FindKey(InKey) >= 0 ? Board->GetNumber(InKey) : InDefault;
    }

    bool SetAIKey(GameCore::World& InWorld, const GameCore::EntityID InEntity, const char* InKey, const char* InValue)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(InEntity) : nullptr;
        const int32_t Key = Board ? Board->FindKey(InKey) : AICore::Blackboard::INVALID_KEY;
        if(Key < 0) { return false; }
        Board->SetName(Key, InValue ? InValue : "");
        return true;
    }

    bool SetAIKeyEntity(GameCore::World& InWorld, const GameCore::EntityID InEntity, const char* InKey, const GameCore::EntityID InValue)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(InEntity) : nullptr;
        if(!Board || Board->FindKey(InKey) < 0) { return false; }
        Board->SetEntity(InKey, InValue);
        return true;
    }

#else // !WITH_AICORE

    namespace AIDetail
    {
        void AttachLevelAI(GameCore::World&) {}
        void DetachLevelAI(GameCore::World&) {}
        void DrawAIDebug(GameCore::World&, RenderCore::Renderer&) {}
    }

    float SteerInput(GameCore::World&, GameCore::EntityID, const SteerInputMap&, float, float, InputSnapshot&) { return 0.0f; }
    bool  MoveAIAgent(GameCore::World&, GameCore::EntityID, const EngineCore::Vector3&) { return false; }
    void  StopAIAgent(GameCore::World&, GameCore::EntityID) {}
    void  ReportAINoise(GameCore::World&, const EngineCore::Vector3&, float, GameCore::EntityID) {}
    bool  RandomNavPoint(GameCore::World&, const EngineCore::Vector3&, float, EngineCore::Vector3&) { return false; }
    float NavPathLength(GameCore::World&, const EngineCore::Vector3&, const EngineCore::Vector3&) { return -1.0f; }
    bool  BuildNavigation(GameCore::World&, const char*, std::string* OutReport) { if(OutReport) { *OutReport = "AICore is compiled out"; } return false; }
    double GetAIKeyNumber(GameCore::World&, GameCore::EntityID, const char*, const double InDefault) { return InDefault; }
    bool  SetAIKey(GameCore::World&, GameCore::EntityID, const char*, const char*) { return false; }
    bool  SetAIKeyEntity(GameCore::World&, GameCore::EntityID, const char*, GameCore::EntityID) { return false; }

#endif // WITH_AICORE
}
