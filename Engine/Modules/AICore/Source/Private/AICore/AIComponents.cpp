#include "Public/AICore/Components/AIBrainComponent.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/Components/NavMeshVolumeComponent.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/AICore/Components/NavObstacleComponent.h"
#include "Public/AICore/Components/PerceptionComponent.h"

#include <cmath>
#include <cstring>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* AICore's components: registration, saves, the editor's fields and the
 * script fields. What each does is in its header. */

namespace AICore
{
    // === Nav Mesh Volume ======================================================

    GAMECORE_DEFINE_COMPONENT(NavMeshVolumeComponent, "Nav Mesh Volume")

    NavBuildSettings NavMeshVolumeComponent::GetSettings() const
    {
        NavBuildSettings S;
        S.CellSize = CellSize;
        S.CellHeight = CellHeight;
        S.AgentHeight = AgentHeight;
        S.AgentRadius = AgentRadius;
        S.AgentMaxClimb = AgentMaxClimb;
        S.AgentMaxSlope = AgentMaxSlope;
        S.TileSize = TileSize;
        S.RegionMinSize = RegionMinSize;
        S.RegionMergeSize = RegionMergeSize;
        S.EdgeMaxLength = EdgeMaxLength;
        S.EdgeMaxError = EdgeMaxError;
        S.LayerMask = LayerMask;
        return S;
    }

    void NavMeshVolumeComponent::SetSettings(const NavBuildSettings& InSettings)
    {
        CellSize = InSettings.CellSize;
        CellHeight = InSettings.CellHeight;
        AgentHeight = InSettings.AgentHeight;
        AgentRadius = InSettings.AgentRadius;
        AgentMaxClimb = InSettings.AgentMaxClimb;
        AgentMaxSlope = InSettings.AgentMaxSlope;
        TileSize = InSettings.TileSize;
        RegionMinSize = InSettings.RegionMinSize;
        RegionMergeSize = InSettings.RegionMergeSize;
        EdgeMaxLength = InSettings.EdgeMaxLength;
        EdgeMaxError = InSettings.EdgeMaxError;
        LayerMask = InSettings.LayerMask;
    }

    GameCore::AABB NavMeshVolumeComponent::GetWorldBounds() const
    {
        const GameCore::TransformPose Pose = Owner.GetEntity().GetWorldPose();
        const EngineCore::Vector3 Half(std::fabs(float(HalfExtents.GetX() * Pose.Scale.GetX())),
                                       std::fabs(float(HalfExtents.GetY() * Pose.Scale.GetY())),
                                       std::fabs(float(HalfExtents.GetZ() * Pose.Scale.GetZ())));
        return GameCore::AABB(Pose.Position - Half, Pose.Position + Half);
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NavMeshVolumeComponent, 1)
    {
        Ar.Field("HalfExtents",     C.HalfExtents);
        Ar.Field("CellSize",        C.CellSize);
        Ar.Field("CellHeight",      C.CellHeight);
        Ar.Field("AgentHeight",     C.AgentHeight);
        Ar.Field("AgentRadius",     C.AgentRadius);
        Ar.Field("AgentMaxClimb",   C.AgentMaxClimb);
        Ar.Field("AgentMaxSlope",   C.AgentMaxSlope);
        Ar.Field("TileSize",        C.TileSize);
        Ar.Field("RegionMinSize",   C.RegionMinSize);
        Ar.Field("RegionMergeSize", C.RegionMergeSize);
        Ar.Field("EdgeMaxLength",   C.EdgeMaxLength);
        Ar.Field("EdgeMaxError",    C.EdgeMaxError);
        Ar.Field("LayerMask",       C.LayerMask);
        Ar.Field("NavMesh",         C.NavMesh);
        Ar.Flag("BuildAtRuntime",   C.bBuildAtRuntime);
        Ar.Flag("RebuildOnChange",  C.bRebuildOnChange);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NavMeshVolumeComponent)
    {
        Bind.Field("HalfExtents", &NavMeshVolumeComponent::HalfExtents);
        Bind.Field("AgentRadius", &NavMeshVolumeComponent::AgentRadius);
        Bind.Field("AgentHeight", &NavMeshVolumeComponent::AgentHeight);
        Bind.Field("NavMesh",     &NavMeshVolumeComponent::NavMesh);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NavMeshVolumeComponent)
    {
        Props.Category("Volume");
        Props.Field("Half Extents",      &NavMeshVolumeComponent::HalfExtents);
        Props.Field("Nav Mesh",          &NavMeshVolumeComponent::NavMesh, GameCore::PF_AssetNavMesh);
        Props.Flag("Build At Runtime",   &NavMeshVolumeComponent::bBuildAtRuntime);
        Props.Flag("Rebuild On Change",  &NavMeshVolumeComponent::bRebuildOnChange);
        Props.Category("Agent");
        Props.Field("Agent Radius",      &NavMeshVolumeComponent::AgentRadius);
        Props.Field("Agent Height",      &NavMeshVolumeComponent::AgentHeight);
        Props.Field("Agent Max Climb",   &NavMeshVolumeComponent::AgentMaxClimb);
        Props.Field("Agent Max Slope",   &NavMeshVolumeComponent::AgentMaxSlope);
        Props.Category("Voxels and Tiles");
        Props.Field("Cell Size",         &NavMeshVolumeComponent::CellSize);
        Props.Field("Cell Height",       &NavMeshVolumeComponent::CellHeight);
        Props.Field("Tile Size",         &NavMeshVolumeComponent::TileSize);
        Props.Field("Region Min Size",   &NavMeshVolumeComponent::RegionMinSize);
        Props.Field("Region Merge Size", &NavMeshVolumeComponent::RegionMergeSize);
        Props.Field("Edge Max Length",   &NavMeshVolumeComponent::EdgeMaxLength);
        Props.Field("Edge Max Error",    &NavMeshVolumeComponent::EdgeMaxError);
        Props.Field("Layer Mask",        &NavMeshVolumeComponent::LayerMask);
    }
#endif

    // === Nav Modifier =========================================================

    GAMECORE_DEFINE_COMPONENT(NavModifierComponent, "Nav Modifier")

    namespace
    {
        const char* const GModifierAreaNames[] = { "Blocked", "HighCost", "Custom" };
    }

    uint8_t NavModifierComponent::GetAreaId() const
    {
        switch(Area)
        {
            case NMA_Blocked:  return NAV_AREA_BLOCKED;
            case NMA_HighCost: return NAV_AREA_HIGH_COST;
            default:
                return CustomArea >= NAV_AREA_CUSTOM_MIN && CustomArea <= NAV_AREA_CUSTOM_MAX ? CustomArea : uint8_t(NAV_AREA_CUSTOM_MIN);
        }
    }

    const char* NavModifierComponent::GetAreaName() const
    {
        return Area <= NMA_Custom ? GModifierAreaNames[Area] : "Blocked";
    }

    void NavModifierComponent::SetAreaName(const char* InName)
    {
        for(uint8_t Index = 0; Index <= NMA_Custom; ++Index)
        {
            if(InName && std::strcmp(InName, GModifierAreaNames[Index]) == 0) { Area = Index; return; }
        }
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NavModifierComponent, 1)
    {
        Ar.Field("HalfExtents", C.HalfExtents);
        Ar.Field("Area",        C.Area);
        Ar.Field("CustomArea",  C.CustomArea);
        Ar.Field("Cost",        C.Cost);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NavModifierComponent)
    {
        Bind.Field("HalfExtents", &NavModifierComponent::HalfExtents);
        Bind.Field("Cost",        &NavModifierComponent::Cost);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NavModifierComponent)
    {
        Props.Category("Nav Modifier");
        Props.Field("Half Extents", &NavModifierComponent::HalfExtents);
        Props.Choice("Area",        &NavModifierComponent::Area, "Blocked|High cost|Custom");
        Props.Field("Custom Area",  &NavModifierComponent::CustomArea);
        Props.Field("Cost",         &NavModifierComponent::Cost);
    }
#endif

    // === Nav Obstacle =========================================================

    GAMECORE_DEFINE_COMPONENT(NavObstacleComponent, "Nav Obstacle")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NavObstacleComponent, 1)
    {
        Ar.Field("HalfExtents", C.HalfExtents);
        Ar.Field("Shape",       C.Shape);
        Ar.Flag("Enabled",      C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NavObstacleComponent)
    {
        Bind.Field("HalfExtents", &NavObstacleComponent::HalfExtents);
        Bind.Flag("Enabled",      &NavObstacleComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NavObstacleComponent)
    {
        Props.Category("Nav Obstacle");
        Props.Field("Half Extents", &NavObstacleComponent::HalfExtents);
        Props.Choice("Shape",       &NavObstacleComponent::Shape, "Box|Cylinder");
        Props.Flag("Enabled",       &NavObstacleComponent::bEnabled);
    }
#endif

    // === Nav Agent ============================================================

    GAMECORE_DEFINE_COMPONENT(NavAgentComponent, "Nav Agent")

    float GetFacingYaw(const GameCore::Entity& InEntity)
    {
        const NavAgentComponent* Agent = InEntity.GetComponent<NavAgentComponent>();
        float Yaw = float(InEntity.GetWorldPose().Rotation.GetY()) - (Agent ? Agent->MeshYaw : 0.0f);
        while(Yaw > 180.0f)  { Yaw -= 360.0f; }
        while(Yaw < -180.0f) { Yaw += 360.0f; }
        return Yaw;
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NavAgentComponent, 2)
    {
        Ar.Field("Radius",           C.Radius);
        Ar.Field("Height",           C.Height);
        Ar.Field("MaxSpeed",         C.MaxSpeed);
        Ar.Field("MaxAcceleration",  C.MaxAcceleration);
        Ar.Field("AcceptanceRadius", C.AcceptanceRadius);
        Ar.Field("SeparationWeight", C.SeparationWeight);
        Ar.Field("AvoidanceQuality", C.AvoidanceQuality);
        Ar.Flag("DriveTransform",    C.bDriveTransform);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("MeshYaw",  C.MeshYaw);
            Ar.Field("TurnRate", C.TurnRate);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NavAgentComponent)
    {
        Bind.Field("MaxSpeed",            &NavAgentComponent::MaxSpeed);
        Bind.Field("AcceptanceRadius",    &NavAgentComponent::AcceptanceRadius);
        Bind.Flag("DriveTransform",       &NavAgentComponent::bDriveTransform);
        Bind.ReadOnly("Status",           &NavAgentComponent::Status);
        Bind.ReadOnly("DesiredVelocity",  &NavAgentComponent::DesiredVelocity);
        Bind.ReadOnly("Target",           &NavAgentComponent::Target);
        Bind.ReadOnly("RemainingDistance", &NavAgentComponent::RemainingDistance);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NavAgentComponent)
    {
        Props.Category("Nav Agent");
        Props.Field("Radius",            &NavAgentComponent::Radius);
        Props.Field("Height",            &NavAgentComponent::Height);
        Props.Field("Max Speed",         &NavAgentComponent::MaxSpeed);
        Props.Field("Max Acceleration",  &NavAgentComponent::MaxAcceleration);
        Props.Field("Acceptance Radius", &NavAgentComponent::AcceptanceRadius);
        Props.Field("Separation Weight", &NavAgentComponent::SeparationWeight);
        Props.Choice("Avoidance",        &NavAgentComponent::AvoidanceQuality, "None|Low|Medium|High");
        Props.Flag("Drive Transform",    &NavAgentComponent::bDriveTransform);
        Props.Field("Mesh Yaw",          &NavAgentComponent::MeshYaw);
        Props.Field("Turn Rate",         &NavAgentComponent::TurnRate);
    }
#endif

    // === Perception ===========================================================

    GAMECORE_DEFINE_COMPONENT(PerceptionComponent, "Perception")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PerceptionComponent, 1)
    {
        Ar.Field("SightRange",    C.SightRange);
        Ar.Field("FovHalfAngle",  C.FovHalfAngle);
        Ar.Field("EyeHeight",     C.EyeHeight);
        Ar.Field("HearingRange",  C.HearingRange);
        Ar.Field("ForgetSeconds", C.ForgetSeconds);
        Ar.Field("SightInterval", C.SightInterval);
        Ar.Field("Team",          C.Team);
        Ar.Flag("Enabled",        C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(PerceptionComponent)
    {
        Bind.Field("SightRange",   &PerceptionComponent::SightRange);
        Bind.Field("HearingRange", &PerceptionComponent::HearingRange);
        Bind.Field("Team",         &PerceptionComponent::Team);
        Bind.Flag("Enabled",       &PerceptionComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PerceptionComponent)
    {
        Props.Category("Perception");
        Props.Field("Sight Range",    &PerceptionComponent::SightRange);
        Props.Field("FOV Half Angle", &PerceptionComponent::FovHalfAngle);
        Props.Field("Eye Height",     &PerceptionComponent::EyeHeight);
        Props.Field("Hearing Range",  &PerceptionComponent::HearingRange);
        Props.Field("Forget Seconds", &PerceptionComponent::ForgetSeconds);
        Props.Field("Sight Interval", &PerceptionComponent::SightInterval);
        Props.Field("Team",           &PerceptionComponent::Team);
        Props.Flag("Enabled",         &PerceptionComponent::bEnabled);
    }
#endif

    // === AI Stimulus ==========================================================

    GAMECORE_DEFINE_COMPONENT(AIStimulusComponent, "AI Stimulus")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AIStimulusComponent, 1)
    {
        Ar.Field("Team",         C.Team);
        Ar.Flag("Enabled",       C.bEnabled);
        Ar.Field("TargetHeight", C.TargetHeight);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(AIStimulusComponent)
    {
        Bind.Field("Team",   &AIStimulusComponent::Team);
        Bind.Flag("Enabled", &AIStimulusComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AIStimulusComponent)
    {
        Props.Category("AI Stimulus");
        Props.Field("Team",          &AIStimulusComponent::Team);
        Props.Flag("Enabled",        &AIStimulusComponent::bEnabled);
        Props.Field("Target Height", &AIStimulusComponent::TargetHeight);
    }
#endif

    // === AI Brain =============================================================

    GAMECORE_DEFINE_COMPONENT(AIBrainComponent, "AI Brain")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AIBrainComponent, 1)
    {
        Ar.Field("Tree",         C.Tree);
        Ar.Field("Blackboard",   C.Blackboard);
        Ar.Field("TickInterval", C.TickInterval);
        Ar.Flag("AutoStart",     C.bAutoStart);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(AIBrainComponent)
    {
        Bind.Field("Tree",           &AIBrainComponent::Tree);
        Bind.Field("TickInterval",   &AIBrainComponent::TickInterval);
        Bind.Flag("AutoStart",       &AIBrainComponent::bAutoStart);
        Bind.Flag("Running",         &AIBrainComponent::bRunning, true);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AIBrainComponent)
    {
        Props.Category("AI Brain");
        Props.Field("Tree",          &AIBrainComponent::Tree, GameCore::PF_AssetBehaviorTree);
        Props.Field("Blackboard",    &AIBrainComponent::Blackboard);
        Props.Field("Tick Interval", &AIBrainComponent::TickInterval);
        Props.Flag("Auto Start",     &AIBrainComponent::bAutoStart);
    }
#endif
}
