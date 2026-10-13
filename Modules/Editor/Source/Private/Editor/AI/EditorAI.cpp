#include "Private/Editor/AI/EditorAI.h"

#if WITH_EDITOR && WITH_AICORE

#include <cmath>

#include <QDir>
#include <QFileInfo>

#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Particles/ParticleAssets.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/Components/NavMeshVolumeComponent.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/AICore/Components/NavObstacleComponent.h"
#include "Public/AICore/Components/PerceptionComponent.h"
#include "Public/AICore/NavDebugDraw.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderTypes.h"

/* EditorAI.h has the contract. */
namespace EditorAI
{
    namespace
    {
        Host            GHost;
        OverlaySettings GOverlay;
        TestPath        GTestPath;
        GameCore::EntityID GDebugged;
        GameCore::World*   GAttached = nullptr;   // the edited World we attached an AIWorld to
        bool               bOverlayInstalled = false;
        OverlayStats       GStats, GCounting;
        int*               GBucket = nullptr;          // what Line() counts into

        void DetachEdited()
        {
            if(GAttached) { AICore::AIWorld::Detach(*GAttached); }
            GAttached = nullptr;
        }

        EngineCore::Color ToColor(const uint32_t InArgb)
        {
            return EngineCore::Color(uint8_t((InArgb >> 16) & 0xFF), uint8_t((InArgb >> 8) & 0xFF), uint8_t(InArgb & 0xFF), uint8_t((InArgb >> 24) & 0xFF));
        }

        void Line(EditorContext& InContext, const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const uint32_t InColor,
                  const float InThickness = 1.5f)
        {
            RenderCore::DebugLine L;
            L.Start = InA;
            L.End = InB;
            L.Color = ToColor(InColor);
            L.ThicknessPixels = InThickness;
            InContext.DrawDebugLine(L);
            if(GBucket) { ++*GBucket; }
        }

        /* A box about InCentre turned InYaw degrees: its twelve edges. */
        void YawBox(EditorContext& InContext, const EngineCore::Vector3& InCentre, const EngineCore::Vector3& InHalf, const float InYaw,
                    const uint32_t InColor)
        {
            const double Yaw = double(InYaw) * 3.14159265358979323846 / 180.0;
            const double C = std::cos(Yaw), S = std::sin(Yaw);
            EngineCore::Vector3 Corners[8];
            for(int Index = 0; Index < 8; ++Index)
            {
                const double X = (Index & 1 ? 1.0 : -1.0) * InHalf.GetX(), Y = (Index & 2 ? 1.0 : -1.0) * InHalf.GetY();
                const double Z = (Index & 4 ? 1.0 : -1.0) * InHalf.GetZ();
                Corners[Index] = EngineCore::Vector3(InCentre.GetX() + X * C + Z * S, InCentre.GetY() + Y, InCentre.GetZ() - X * S + Z * C);
            }
            const int Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for(const auto& Edge : Edges) { Line(InContext, Corners[Edge[0]], Corners[Edge[1]], InColor); }
        }

        void DrawAgents(EditorContext& InContext, AICore::AIWorld& InAI)
        {
            GameCore::World& World = InAI.GetWorld();
            std::vector<EngineCore::Vector3> Corners;
            std::vector<AICore::NavDebugLine> Lines;
            World.Each<AICore::NavAgentComponent>([&](const GameCore::Entity& InEntity, AICore::NavAgentComponent& InAgent)
            {
                const bool bFocus = InEntity.GetID() == GDebugged;
                GBucket = &GCounting.Paths;
                if(GOverlay.bPaths && InAI.GetNav().GetAgentCorners(InEntity.GetID(), Corners) > 1)
                {
                    Lines.clear();
                    AICore::DrawNavPath(Corners, Lines, bFocus ? 0xFFFFD040 : 0xA0FFD040);
                    for(const AICore::NavDebugLine& Each : Lines) { Line(InContext, Each.From, Each.To, Each.Color, bFocus ? 2.5f : 1.5f); }
                }
                (void)InAgent;
            });
            if(!GOverlay.bSight) { return; }
            GBucket = &GCounting.Sight;
            World.Each<AICore::PerceptionComponent>([&](const GameCore::Entity& InEntity, AICore::PerceptionComponent& InSight)
            {
                if(!InSight.bEnabled) { return; }
                const bool bFocus = InEntity.GetID() == GDebugged;
                if(GDebugged.IsSet() && !bFocus) { return; }
                const EngineCore::Vector3 Feet = InEntity.GetWorldPose().Position;
                const EngineCore::Vector3 Eye(Feet.GetX(), Feet.GetY() + InSight.EyeHeight, Feet.GetZ());
                /* The cone: two edges and an arc at the sight range. */
                const double Yaw = double(AICore::GetFacingYaw(InEntity)) * 3.14159265358979323846 / 180.0;
                const double Half = double(InSight.FovHalfAngle) * 3.14159265358979323846 / 180.0;
                const double Range = std::min(double(InSight.SightRange), 2500.0);
                EngineCore::Vector3 Previous;
                for(int Step = 0; Step <= 12; ++Step)
                {
                    const double A = Yaw - Half + 2.0 * Half * Step / 12.0;
                    const EngineCore::Vector3 Point(Eye.GetX() + std::sin(A) * Range, Eye.GetY(), Eye.GetZ() + std::cos(A) * Range);
                    if(Step == 0 || Step == 12) { Line(InContext, Eye, Point, 0x804FC3F7); }
                    if(Step > 0) { Line(InContext, Previous, Point, 0x804FC3F7); }
                    Previous = Point;
                }
                /* What it perceives: seen green, remembered or heard amber. */
                if(const std::vector<AICore::PerceivedStimulus>* Memory = InAI.GetPerception().GetMemory(InEntity.GetID()))
                {
                    for(const AICore::PerceivedStimulus& S : *Memory)
                    {
                        const EngineCore::Vector3 At(S.LastKnownPosition.GetX(), S.LastKnownPosition.GetY() + 110.0, S.LastKnownPosition.GetZ());
                        Line(InContext, Eye, At, S.bSeen ? 0xFF4CAF50 : 0xFFFFB300, S.bSeen ? 2.0f : 1.0f);
                    }
                }
            });
        }

        void QueueOverlay(ViewportWidget& InViewport)
        {
            EditorContext& Context = InViewport.GetContext();
            GStats = GCounting;
            GCounting = OverlayStats();
            AICore::AIWorld* AI = GetAI();
            if(!AI) { return; }
            GameCore::World& World = AI->GetWorld();

            /* Each flag on its own: tile bounds and obstacles draw without
             * the polygons too. */
            if(AI->GetNav().HasNavMesh())
            {
                const auto Draw = [&](const uint32_t InFlags, int& InBucket)
                {
                    std::vector<AICore::NavDebugLine> Lines;
                    AICore::DrawNavMesh(AI->GetNav().GetNavMesh(), Lines, InFlags);
                    GBucket = &InBucket;
                    for(const AICore::NavDebugLine& Each : Lines) { Line(Context, Each.From, Each.To, Each.Color, 1.0f); }
                };
                if(GOverlay.bNavMesh)    { Draw(AICore::NDF_Default, GCounting.NavMesh); }
                if(GOverlay.bTileBounds) { Draw(AICore::NDF_TileBounds, GCounting.TileBounds); }
            }
            /* Obstacles by their components, so the edited level shows them
             * too (its World never ticks to cut them): red while they block,
             * a faint amber while open. */
            if(GOverlay.bObstacles)
            {
                GBucket = &GCounting.Obstacles;
                World.Each<AICore::NavObstacleComponent>([&](const GameCore::Entity& InEntity, AICore::NavObstacleComponent& InObstacle)
                {
                    const GameCore::TransformPose Pose = InEntity.GetWorldPose();
                    const EngineCore::Vector3 Half(std::fabs(InObstacle.HalfExtents.GetX() * Pose.Scale.GetX()), std::fabs(InObstacle.HalfExtents.GetY() * Pose.Scale.GetY()),
                                                   std::fabs(InObstacle.HalfExtents.GetZ() * Pose.Scale.GetZ()));
                    const uint32_t Color = InObstacle.bEnabled ? 0xFFE63946 : 0x80FFB703;
                    if(InObstacle.Shape != AICore::NOS_Cylinder)
                    {
                        YawBox(Context, Pose.Position, Half, float(Pose.Rotation.GetY()), Color);
                        return;
                    }
                    const double Radius = Half.GetX();
                    const EngineCore::Vector3 Up(0.0, Half.GetY(), 0.0);
                    EngineCore::Vector3 Previous;
                    for(int Step = 0; Step <= 16; ++Step)
                    {
                        const double Angle = double(Step) / 16.0 * 6.283185307179586;
                        const EngineCore::Vector3 Rim = Pose.Position + EngineCore::Vector3(std::cos(Angle) * Radius, 0.0, std::sin(Angle) * Radius);
                        if(Step > 0)
                        {
                            Line(Context, Previous - Up, Rim - Up, Color);
                            Line(Context, Previous + Up, Rim + Up, Color);
                        }
                        if(Step % 4 == 0) { Line(Context, Rim - Up, Rim + Up, Color); }
                        Previous = Rim;
                    }
                });
            }
            GBucket = &GCounting.Modifiers;
            if(GOverlay.bModifiers && (GOverlay.bNavMesh || !IsPlaying()))
            {
                World.Each<AICore::NavModifierComponent>([&](const GameCore::Entity& InEntity, AICore::NavModifierComponent& InModifier)
                {
                    const GameCore::TransformPose Pose = InEntity.GetWorldPose();
                    const EngineCore::Vector3 Half(InModifier.HalfExtents.GetX() * Pose.Scale.GetX(), InModifier.HalfExtents.GetY() * Pose.Scale.GetY(),
                                                   InModifier.HalfExtents.GetZ() * Pose.Scale.GetZ());
                    YawBox(Context, Pose.Position, Half, float(Pose.Rotation.GetY()), AICore::GetNavAreaColor(InModifier.GetAreaId()));
                });
                World.Each<AICore::NavMeshVolumeComponent>([&](const GameCore::Entity&, AICore::NavMeshVolumeComponent& InVolume)
                {
                    const GameCore::AABB Box = InVolume.GetWorldBounds();
                    const EngineCore::Vector3 Centre = (Box.Min + Box.Max) * 0.5f, Half = (Box.Max - Box.Min) * 0.5f;
                    YawBox(Context, Centre, Half, 0.0f, 0x6080DEEA);
                });
            }
            GBucket = &GCounting.TestPath;
            if(GTestPath.bHasStart || GTestPath.bHasEnd)
            {
                const auto Cross = [&](const EngineCore::Vector3& InAt, const uint32_t InColor)
                {
                    const EngineCore::Vector3 Up(0.0, 60.0, 0.0);
                    Line(Context, InAt, InAt + Up, InColor, 3.0f);
                    Line(Context, InAt - EngineCore::Vector3(25.0, 0.0, 0.0), InAt + EngineCore::Vector3(25.0, 0.0, 0.0), InColor, 3.0f);
                    Line(Context, InAt - EngineCore::Vector3(0.0, 0.0, 25.0), InAt + EngineCore::Vector3(0.0, 0.0, 25.0), InColor, 3.0f);
                };
                if(GTestPath.bHasStart) { Cross(GTestPath.Start, 0xFF66BB6A); }
                if(GTestPath.bHasEnd)   { Cross(GTestPath.End, 0xFFEF5350); }
                std::vector<AICore::NavDebugLine> Lines;
                AICore::DrawNavPath(GTestPath.Points, Lines, GTestPath.Status == int(AICore::ENavPathStatus::Complete) ? 0xFFFFD040 : 0xFFFF7043);
                for(const AICore::NavDebugLine& Each : Lines) { Line(Context, Each.From, Each.To, Each.Color, 3.0f); }
            }
            DrawAgents(Context, *AI);
            GBucket = nullptr;
        }
    }

    void SetHost(const Host& InHost)
    {
        GHost = InHost;
        if(GHost.Document)
        {
            QObject::connect(GHost.Document, &EditorDocument::EditedWorldAboutToBeDestroyed, GHost.Document, []() { DetachEdited(); });
        }
    }

    const Host& GetHost() { return GHost; }

    bool IsPlaying()
    {
        return GHost.Context && GHost.Context->IsPlaying();
    }

    AICore::AIWorld* GetAI()
    {
        if(!GHost.Document) { return nullptr; }
        GameCore::World& World = GHost.Document->GetWorld();
        if(AICore::AIWorld* Existing = AICore::AIWorld::Find(&World)) { return Existing; }
        if(IsPlaying()) { return nullptr; }   // a playing level without AI (a client)
        DetachEdited();
        GAttached = &World;
        return AICore::AIWorld::Attach(World);
    }

    OverlaySettings& GetOverlaySettings() { return GOverlay; }
    const OverlayStats& GetOverlayStats() { return GStats; }
    TestPath&        GetTestPath() { return GTestPath; }
    GameCore::EntityID& GetDebuggedAgent() { return GDebugged; }

    void UpdateTestPath()
    {
        GTestPath.Points.clear();
        GTestPath.Status = int(AICore::ENavPathStatus::None);
        GTestPath.Length = 0.0f;
        AICore::AIWorld* AI = GetAI();
        if(!AI || !GTestPath.bHasStart || !GTestPath.bHasEnd || !AI->GetNav().HasNavMesh()) { return; }
        GTestPath.Status = int(AI->GetNav().GetQuery().FindPath(GTestPath.Start, GTestPath.End, GTestPath.Points, &AI->GetNav().GetDefaultFilter()));
        GTestPath.Length = AICore::NavQuery::GetPathLength(GTestPath.Points);
    }

    void InstallOverlay()
    {
        if(bOverlayInstalled) { return; }
        bOverlayInstalled = true;
        ViewportWidget::AddOverlay([](ViewportWidget& InViewport) { QueueOverlay(InViewport); }, true);
    }

    bool GetNavMeshPaths(QString& OutRuntime, QString& OutDisk)
    {
        AICore::AIWorld* AI = GetAI();
        if(!AI) { return false; }
        QString Runtime;
        AI->GetWorld().Each<AICore::NavMeshVolumeComponent>([&](const GameCore::Entity&, AICore::NavMeshVolumeComponent& InVolume)
        {
            if(Runtime.isEmpty()) { Runtime = QString::fromUtf8(InVolume.NavMesh); }
        });
        if(Runtime.isEmpty())
        {
            /* Beside the level, in its entity folder: Content/Levels/Arena/Arena.nmnav. */
            const QString WorldFile = GHost.GetWorldFile ? GHost.GetWorldFile() : QString();
            if(WorldFile.isEmpty()) { return false; }
            const QFileInfo Info(WorldFile);
            const QString Folder = Info.absolutePath() + QStringLiteral("/") + Info.completeBaseName();
            Runtime = EditorContent::ToRuntimePath(Folder + QStringLiteral("/") + Info.completeBaseName() + QStringLiteral(".nmnav"));
            if(Runtime.isEmpty()) { return false; }
        }
        OutRuntime = Runtime;
        OutDisk = EditorParticles::ToProjectPath(Runtime);
        return !OutDisk.isEmpty();
    }
}

#endif // WITH_EDITOR && WITH_AICORE
