#if WITH_EDITOR

#include "Private/Editor/Procedural/ProceduralEditing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <QKeyEvent>

#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportTool.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/Components/SplineComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"
#include "Public/RenderCore/ProceduralGeneration.h"
#include "Public/RenderCore/RenderTypes.h"

namespace
{
    using EngineCore::Vector3;

    const EngineCore::Color SplineColor(70, 160, 200);
    const EngineCore::Color SelectedSplineColor(255, 165, 40);
    const EngineCore::Color BandColor(200, 130, 40);
    const EngineCore::Color PointColor(255, 220, 120);
    const EngineCore::Color ActivePointColor(255, 255, 255);
    const EngineCore::Color SpawnerBoxColor(120, 220, 120);

    constexpr double PointPickPixels = 10.0;

    EditorProcedural::Host GHost;
    std::function<void(bool)> GEditingChanged;

    Vector3 TransformPoint(const EngineCore::Matrix4x4& InMatrix, const Vector3& InPoint)
    {
        float Out[3];
        for(int Row = 0; Row < 3; ++Row)
        {
            Out[Row] = float(InMatrix.Data[Row][0] * InPoint.GetX() + InMatrix.Data[Row][1] * InPoint.GetY()
                           + InMatrix.Data[Row][2] * InPoint.GetZ() + InMatrix.Data[Row][3]);
        }
        return Vector3(Out[0], Out[1], Out[2]);
    }

    void Line(const Vector3& InA, const Vector3& InB, const EngineCore::Color InColor, const float InThickness)
    {
        RenderCore::DebugLine Each;
        Each.Start = InA;
        Each.End   = InB;
        Each.Color = InColor;
        Each.ThicknessPixels = InThickness;
        GHost.Context->DrawDebugLine(Each);
    }

    /* The selected entity's Spline, or null. */
    GameCore::SplineComponent* SelectedSpline(GameCore::EntityID* OutEntity = nullptr)
    {
        if(!GHost.Document) { return nullptr; }
        const GameCore::EntityID Selected = GHost.Document->GetSelection();
        if(!Selected.IsSet() || !GHost.Document->GetWorld().IsAlive(Selected)) { return nullptr; }
        if(OutEntity) { *OutEntity = Selected; }
        return GHost.Document->GetWorld().GetEntity(Selected).GetComponent<GameCore::SplineComponent>();
    }

    /* InEntity's Spline, added (one undo step) if it has none. */
    GameCore::SplineComponent* EnsureSpline(const GameCore::EntityID InEntity)
    {
        GameCore::Entity Self = GHost.Document->GetWorld().GetEntity(InEntity);
        if(GameCore::SplineComponent* Found = Self.GetComponent<GameCore::SplineComponent>()) { return Found; }
        GHost.Document->AddComponent(InEntity, GameCore::SplineComponent::StaticType().ID);
        return Self.GetComponent<GameCore::SplineComponent>();
    }

    /* One recorded change of InEntity's InType: InEdit runs between the gesture's ends. */
    template<typename F>
    void RecordEdit(const GameCore::EntityID InEntity, const GameCore::ComponentTypeID InType, const QString& InLabel, F&& InEdit)
    {
        if(GHost.Document->GetSelection() != InEntity) { GHost.Document->SetSelection(InEntity); }
        GHost.Document->BeginEditGesture(InLabel);
        InEdit();
        GHost.Document->NotifyPropertyEdited(InEntity, InType);
        GHost.Document->EndEditGesture();
    }

    /*
     * The point editor (see the header). The selected entity is its target;
     * its points are moved in that entity's space, found from where the
     * cursor meets the scene.
     */
    class SplineTool final : public ViewportTool
    {
    public:
        int32_t SelectedPoint = -1;
        int32_t HoverPoint    = -1;

        bool OnPress(ViewportWidget& InViewport, const Ray& InRay, const Qt::KeyboardModifiers InModifiers) override
        {
            GameCore::EntityID Entity;
            GameCore::SplineComponent* Spline = SelectedSpline(&Entity);
            if(!Spline) { return false; }
            if(Entity != Target)
            {
                Target        = Entity;
                SelectedPoint = -1;
            }
            const GameCore::Entity Self = GHost.Document->GetWorld().GetEntity(Entity);

            const int32_t Hit = PointUnder(InViewport, InRay, *Spline, Self.GetWorldMatrix());
            if(Hit >= 0)
            {
                SelectedPoint = Hit;
                BeginDrag(Entity, *Spline, QStringLiteral("Move Spline Point"));
                return true;
            }
            if(InModifiers & Qt::ControlModifier)
            {
                const int32_t Count = Spline->GetPointCount();
                if(Count >= int32_t(GameCore::SPLINE_POINTS_MAX)) { return true; }
                Vector3 World;
                const float PlaneY = Count > 0 ? float(TransformPoint(Self.GetWorldMatrix(), Spline->GetPoint(Count - 1)).GetY())
                                               : float(Self.GetWorldPose().Position.GetY());
                if(!InViewport.RaycastScene(InRay, World, Entity) && !OnPlane(InRay, PlaneY, World)) { return true; }
                const Vector3 Local = TransformPoint(EngineCore::Matrix4x4::Inverse(Self.GetWorldMatrix()), World);
                const int32_t At = SelectedPoint >= 0 && SelectedPoint + 1 < Count ? SelectedPoint + 1 : Count;
                GHost.Document->BeginEditGesture(QStringLiteral("Add Spline Point"));
                Spline->InsertPoint(At, Local);
                GHost.Document->NotifyPropertyEdited(Entity, GameCore::SplineComponent::StaticType().ID);
                SelectedPoint = At;
                /* Held: it drags on from here, in the same undo step. */
                bDragging   = true;
                DragEntity  = Entity;
                DragStart   = Spline->GetPoint(At);
                DragWorldY  = float(World.GetY());
                bOwnGesture = true;
                return true;
            }
            return false;
        }

        void OnDrag(ViewportWidget& InViewport, const Ray& InRay, const Qt::KeyboardModifiers InModifiers) override
        {
            GameCore::EntityID Entity;
            GameCore::SplineComponent* Spline = SelectedSpline(&Entity);
            if(!bDragging || !Spline || Entity != DragEntity || SelectedPoint < 0) { return; }
            const GameCore::Entity Self = GHost.Document->GetWorld().GetEntity(Entity);
            const EngineCore::Matrix4x4 ToWorld = Self.GetWorldMatrix();
            Vector3 World = TransformPoint(ToWorld, Spline->GetPoint(SelectedPoint));

            if(InModifiers & Qt::ShiftModifier)
            {
                /* Up and down: the point on its vertical line nearest the ray. */
                const Vector3 Up(0.0f, 1.0f, 0.0f);
                const Vector3 W = InRay.Origin - World;
                const double B = Up.Dot(InRay.Direction);
                const double D = Up.Dot(W), E = InRay.Direction.Dot(W);
                const double Denominator = 1.0 - B * B;
                if(std::fabs(Denominator) < 1e-6) { return; }
                const double S = (D - B * E) / Denominator;
                World = Vector3(World.GetX(), float(World.GetY() + S), World.GetZ());
                DragWorldY = float(World.GetY());
            }
            else
            {
                Vector3 Hit;
                if(InViewport.RaycastScene(InRay, Hit, Entity)) { World = Hit; DragWorldY = float(Hit.GetY()); }
                else if(OnPlane(InRay, DragWorldY, Hit))        { World = Hit; }
                else                                             { return; }
            }
            Spline->SetPoint(SelectedPoint, TransformPoint(EngineCore::Matrix4x4::Inverse(ToWorld), World));
            GHost.Document->NotifyPropertyEdited(Entity, GameCore::SplineComponent::StaticType().ID);
        }

        void OnRelease(ViewportWidget& InViewport, const bool bInCancel) override
        {
            (void)InViewport;
            if(!bDragging) { return; }
            GameCore::EntityID Entity;
            GameCore::SplineComponent* Spline = SelectedSpline(&Entity);
            if(bInCancel && Spline && Entity == DragEntity && SelectedPoint >= 0)
            {
                Spline->SetPoint(SelectedPoint, DragStart);
                GHost.Document->NotifyPropertyEdited(Entity, GameCore::SplineComponent::StaticType().ID);
            }
            bDragging = false;
            if(bOwnGesture) { GHost.Document->EndEditGesture(); }
            bOwnGesture = false;
        }

        void OnHover(ViewportWidget& InViewport, const Ray* InRay, const Qt::KeyboardModifiers InModifiers) override
        {
            (void)InModifiers;
            GameCore::EntityID Entity;
            const GameCore::SplineComponent* Spline = SelectedSpline(&Entity);
            if(Entity != Target)
            {
                /* Another entity: its points, none chosen yet. */
                Target        = Entity;
                SelectedPoint = -1;
            }
            HoverPoint = Spline && InRay ? PointUnder(InViewport, *InRay, *Spline, GHost.Document->GetWorld().GetEntity(Entity).GetWorldMatrix()) : -1;
        }

        bool OnKey(ViewportWidget& InViewport, QKeyEvent* InEvent) override
        {
            (void)InViewport;
            if(InEvent->type() != QEvent::KeyPress) { return false; }
            if(InEvent->key() == Qt::Key_Escape && !bDragging)
            {
                EditorProcedural::SetEditingPoints(false);
                return true;
            }
            if(InEvent->key() == Qt::Key_Delete || InEvent->key() == Qt::Key_Backspace)
            {
                GameCore::EntityID Entity;
                GameCore::SplineComponent* Spline = SelectedSpline(&Entity);
                if(!Spline) { return false; }
                /* Taken whether or not a point is selected: Delete must never fall through to deleting the entity. */
                if(bDragging || SelectedPoint < 0 || SelectedPoint >= Spline->GetPointCount()) { return true; }
                const int32_t Remove = SelectedPoint;
                RecordEdit(Entity, GameCore::SplineComponent::StaticType().ID, QStringLiteral("Remove Spline Point"),
                           [Spline, Remove]() { Spline->RemovePoint(Remove); });
                SelectedPoint = std::min(Remove, Spline->GetPointCount() - 1);
                return true;
            }
            return false;
        }

        void Reset()
        {
            SelectedPoint = -1;
            HoverPoint    = -1;
            Target        = GameCore::EntityID();
        }

    private:
        void BeginDrag(const GameCore::EntityID InEntity, const GameCore::SplineComponent& InSpline, const QString& InLabel)
        {
            const GameCore::Entity Self = GHost.Document->GetWorld().GetEntity(InEntity);
            bDragging   = true;
            DragEntity  = InEntity;
            DragStart   = InSpline.GetPoint(SelectedPoint);
            DragWorldY  = float(TransformPoint(Self.GetWorldMatrix(), DragStart).GetY());
            bOwnGesture = true;
            GHost.Document->BeginEditGesture(InLabel);
        }

        /* The point whose handle is under the ray, within a few pixels; -1 for none. */
        static int32_t PointUnder(ViewportWidget& InViewport, const Ray& InRay, const GameCore::SplineComponent& InSpline,
                                  const EngineCore::Matrix4x4& InToWorld)
        {
            int32_t Best = -1;
            double  BestPixels = PointPickPixels;
            for(int32_t Index = 0; Index < InSpline.GetPointCount(); ++Index)
            {
                const Vector3 World = TransformPoint(InToWorld, InSpline.GetPoint(Index));
                const Vector3 ToPoint = World - InRay.Origin;
                const double Along = ToPoint.Dot(InRay.Direction);
                if(Along <= 0.0) { continue; }
                const Vector3 Closest = InRay.Origin + InRay.Direction * Along;
                const double Pixels = (World - Closest).GetLength() / std::max(InViewport.GetWorldPerPixelAt(World), 1e-9);
                if(Pixels < BestPixels)
                {
                    BestPixels = Pixels;
                    Best = Index;
                }
            }
            return Best;
        }

        /* Where InRay crosses the level plane at InY. */
        static bool OnPlane(const Ray& InRay, const float InY, Vector3& OutPoint)
        {
            if(std::fabs(InRay.Direction.GetY()) < 1e-6) { return false; }
            const double T = (double(InY) - InRay.Origin.GetY()) / InRay.Direction.GetY();
            if(T <= 0.0) { return false; }
            OutPoint = InRay.Origin + InRay.Direction * T;
            return true;
        }

        bool               bDragging   = false;
        bool               bOwnGesture = false;
        GameCore::EntityID Target;        // whose points SelectedPoint counts
        GameCore::EntityID DragEntity;
        Vector3            DragStart;     // the point as the drag began, entity space
        float              DragWorldY = 0.0f;
    };

    SplineTool GTool;
    bool       GEditing = false;

    /* The overlay: every spline, the selected one with its handles; a selected Box spawner's box. */
    void QueueOverlay(ViewportWidget& InViewport)
    {
        if(!GHost.Document || !GHost.Context) { return; }
        GameCore::World& World = GHost.Document->GetWorld();
        const GameCore::EntityID Selected = GHost.Document->GetSelection();

        World.Each<GameCore::SplineComponent>([&](const GameCore::Entity& InEntity, GameCore::SplineComponent& InSpline)
        {
            const bool bSelected = InEntity.GetID() == Selected;
            const EngineCore::Matrix4x4 ToWorld = InEntity.GetWorldMatrix();
            const GameCore::SplineCurve Curve = InSpline.GetCurve().Transformed(ToWorld);
            const std::vector<Vector3>& Poly = Curve.GetPolyline();
            for(size_t Index = 1; Index < Poly.size(); ++Index)
            {
                Line(Poly[Index - 1], Poly[Index], bSelected ? SelectedSplineColor : SplineColor, bSelected ? 3.0f : 1.5f);
            }
            if(!bSelected) { return; }

            /* Its Width, half each side. */
            if(InSpline.Width > 0.0f && Curve.IsValid())
            {
                const float Half = 0.5f * InSpline.Width;
                const float Step = std::max(Curve.GetLength() / 200.0f, 25.0f);
                Vector3 PrevL, PrevR;
                for(float D = 0.0f, Index = 0.0f; ; D += Step, Index += 1.0f)
                {
                    const bool bLast = D >= Curve.GetLength();
                    const GameCore::SplineSample S = Curve.SampleAtDistance(std::min(D, Curve.GetLength()));
                    const Vector3 Right = Vector3(float(S.Tangent.GetZ()), 0.0f, float(-S.Tangent.GetX())).GetNormalize();
                    const Vector3 L = S.Position - Right * Half, R = S.Position + Right * Half;
                    if(Index > 0.0f)
                    {
                        Line(PrevL, L, BandColor, 1.5f);
                        Line(PrevR, R, BandColor, 1.5f);
                    }
                    PrevL = L;
                    PrevR = R;
                    if(bLast) { break; }
                }
            }

            /* Its points: a cross each, sized in pixels; the first one larger. */
            for(int32_t Index = 0; Index < InSpline.GetPointCount(); ++Index)
            {
                const Vector3 P = TransformPoint(ToWorld, InSpline.GetPoint(Index));
                const bool bActive = GEditing && (Index == GTool.SelectedPoint || Index == GTool.HoverPoint);
                const double Size = InViewport.GetWorldPerPixelAt(P) * (Index == 0 ? 9.0 : 6.0);
                const EngineCore::Color Color = bActive ? ActivePointColor : PointColor;
                const float Thick = bActive ? 3.0f : 2.0f;
                Line(P - Vector3(float(Size), 0.0f, 0.0f), P + Vector3(float(Size), 0.0f, 0.0f), Color, Thick);
                Line(P - Vector3(0.0f, float(Size), 0.0f), P + Vector3(0.0f, float(Size), 0.0f), Color, Thick);
                Line(P - Vector3(0.0f, 0.0f, float(Size)), P + Vector3(0.0f, 0.0f, float(Size)), Color, Thick);
            }
        });

        if(Selected.IsSet() && World.IsAlive(Selected))
        {
            const GameCore::Entity Self = World.GetEntity(Selected);
            const RenderCore::ProceduralSpawnerComponent* Spawner = Self.GetComponent<RenderCore::ProceduralSpawnerComponent>();
            if(Spawner && Spawner->Source == RenderCore::PGS_Box)
            {
                const EngineCore::Matrix4x4 ToWorld = Self.GetWorldMatrix();
                const Vector3& E = Spawner->BoxExtent;
                Vector3 Corner[8];
                for(int Index = 0; Index < 8; ++Index)
                {
                    Corner[Index] = TransformPoint(ToWorld, Vector3((Index & 1) ? E.GetX() : -E.GetX(), (Index & 2) ? E.GetY() : -E.GetY(),
                                                                    (Index & 4) ? E.GetZ() : -E.GetZ()));
                }
                const int Edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
                for(const auto& Edge : Edges) { Line(Corner[Edge[0]], Corner[Edge[1]], SpawnerBoxColor, 2.0f); }
            }
        }
    }

    bool ParseVector(const QStringList& InArgs, const int InFirst, Vector3& OutValue)
    {
        if(InArgs.size() < InFirst + 3) { return false; }
        bool bX = false, bY = false, bZ = false;
        OutValue = Vector3(InArgs[InFirst].toFloat(&bX), InArgs[InFirst + 1].toFloat(&bY), InArgs[InFirst + 2].toFloat(&bZ));
        return bX && bY && bZ;
    }

    bool ParseFlag(const QString& InText, uint8_t& OutValue)
    {
        const QString Text = InText.toLower();
        if(Text == QStringLiteral("on") || Text == QStringLiteral("true") || Text == QStringLiteral("1"))   { OutValue = 1; return true; }
        if(Text == QStringLiteral("off") || Text == QStringLiteral("false") || Text == QStringLiteral("0")) { OutValue = 0; return true; }
        return false;
    }

    void CopyText(char* OutBuffer, const size_t InSize, const QString& InText)
    {
        const QByteArray Bytes = InText.toUtf8();
        std::memset(OutBuffer, 0, InSize);
        std::memcpy(OutBuffer, Bytes.constData(), std::min(size_t(Bytes.size()), InSize - 1));
    }
}

namespace EditorProcedural
{
    void Install(const Host& InHost)
    {
        GHost = InHost;
        ViewportWidget::AddOverlay([](ViewportWidget& InViewport) { QueueOverlay(InViewport); });
    }

    void SetEditingPoints(const bool bInEditing)
    {
        if(!GHost.Viewport) { return; }
        if(bInEditing)
        {
            GameCore::EntityID Entity;
            SelectedSpline(&Entity);
            if(!Entity.IsSet()) { return; }   // nothing selected to edit
            EnsureSpline(Entity);
            GTool.Reset();
            GHost.Viewport->SetTool(&GTool);
        }
        else if(GHost.Viewport->GetTool() == &GTool)
        {
            GHost.Viewport->SetTool(nullptr);
        }
        const bool bNow = GHost.Viewport->GetTool() == &GTool;
        if(bNow != GEditing)
        {
            GEditing = bNow;
            if(GEditingChanged) { GEditingChanged(GEditing); }
        }
    }

    bool IsEditingPoints()
    {
        return GEditing && GHost.Viewport && GHost.Viewport->GetTool() == &GTool;
    }

    void SetEditingChangedCallback(std::function<void(bool)> InCallback)
    {
        GEditingChanged = std::move(InCallback);
    }

    GameCore::EntityID CreateSpline(const Vector3& InPosition)
    {
        if(!GHost.Document) { return GameCore::EntityID(); }
        const GameCore::EntityID Made = GHost.Document->CreateEntity(QStringLiteral("Spline"), InPosition, [](const GameCore::Entity& InEntity)
        {
            GameCore::SplineComponent Spline;
            Spline.AddPoint(Vector3(0.0f, 0.0f, 0.0f));
            Spline.AddPoint(Vector3(1000.0f, 0.0f, 0.0f));
            InEntity.AddComponent(Spline);
        });
        GHost.Document->SetSelection(Made);
        return Made;
    }

    GameCore::EntityID CreateSpawner(const Vector3& InPosition)
    {
        if(!GHost.Document) { return GameCore::EntityID(); }
        const GameCore::EntityID Made = GHost.Document->CreateEntity(QStringLiteral("Procedural Spawner"), InPosition, [](const GameCore::Entity& InEntity)
        {
            InEntity.AddComponent<RenderCore::ProceduralSpawnerComponent>();
        });
        GHost.Document->SetSelection(Made);
        return Made;
    }

    // === spline ==================================================================

    QString GetSplineUsage()
    {
        return QStringLiteral("<new <name> <x y z> | add <x y z> | insert <i> <x y z> | set <i> <x y z> | remove <i> | clear | "
                              "closed <on|off> | type <curve|linear> | width <w> | edit <on|off> | info>");
    }

    void RunSplineCommand(EditorConsole& InConsole, const QStringList& InArgs)
    {
        if(!GHost.Document || InArgs.isEmpty())
        {
            InConsole.PrintError(QStringLiteral("Usage: spline ") + GetSplineUsage());
            return;
        }
        const QString Verb = InArgs.front().toLower();
        Vector3 Value;
        if(Verb == QStringLiteral("new"))
        {
            if(InArgs.size() < 5 || !ParseVector(InArgs, 2, Value))
            {
                InConsole.PrintError(QStringLiteral("Usage: spline new <name> <x y z>"));
                return;
            }
            const GameCore::EntityID Made = GHost.Document->CreateEntity(InArgs[1], Value, [](const GameCore::Entity& InEntity)
            {
                InEntity.AddComponent<GameCore::SplineComponent>();
            });
            GHost.Document->SetSelection(Made);
            InConsole.Print(QStringLiteral("Made spline '%1'").arg(InArgs[1]));
            return;
        }
        if(Verb == QStringLiteral("edit"))
        {
            uint8_t On = 0;
            if(InArgs.size() < 2 || !ParseFlag(InArgs[1], On)) { InConsole.PrintError(QStringLiteral("Usage: spline edit <on|off>")); return; }
            SetEditingPoints(On != 0);
            InConsole.Print(IsEditingPoints() ? QStringLiteral("Editing spline points") : QStringLiteral("Not editing spline points"));
            return;
        }

        GameCore::EntityID Entity;
        SelectedSpline(&Entity);
        if(!Entity.IsSet())
        {
            InConsole.PrintError(QStringLiteral("Select an entity first"));
            return;
        }
        GameCore::SplineComponent* Spline = EnsureSpline(Entity);
        if(!Spline)
        {
            InConsole.PrintError(QStringLiteral("Could not add a Spline to the selection"));
            return;
        }
        const GameCore::Entity Self = GHost.Document->GetWorld().GetEntity(Entity);
        const EngineCore::Matrix4x4 ToLocal = EngineCore::Matrix4x4::Inverse(Self.GetWorldMatrix());
        const GameCore::ComponentTypeID Type = GameCore::SplineComponent::StaticType().ID;

        if(Verb == QStringLiteral("info"))
        {
            InConsole.Print(QStringLiteral("%1 points, %2 long, %3, %4, width %5")
                                .arg(Spline->GetPointCount()).arg(double(Spline->GetLength()), 0, 'f', 1)
                                .arg(Spline->bClosed ? QStringLiteral("closed") : QStringLiteral("open"))
                                .arg(Spline->Type == GameCore::ST_Linear ? QStringLiteral("linear") : QStringLiteral("curve"))
                                .arg(double(Spline->Width)));
            return;
        }
        if(Verb == QStringLiteral("add") && ParseVector(InArgs, 1, Value))
        {
            const Vector3 Local = TransformPoint(ToLocal, Value);
            bool bDone = false;
            RecordEdit(Entity, Type, QStringLiteral("Add Spline Point"), [&]() { bDone = Spline->AddPoint(Local); });
            if(!bDone) { InConsole.PrintError(QStringLiteral("The spline is full")); }
            return;
        }
        if((Verb == QStringLiteral("insert") || Verb == QStringLiteral("set")) && InArgs.size() >= 5 && ParseVector(InArgs, 2, Value))
        {
            const int32_t Index = InArgs[1].toInt();
            const Vector3 Local = TransformPoint(ToLocal, Value);
            bool bDone = false;
            RecordEdit(Entity, Type, Verb == QStringLiteral("set") ? QStringLiteral("Move Spline Point") : QStringLiteral("Add Spline Point"), [&]()
            {
                bDone = Verb == QStringLiteral("set") ? Spline->SetPoint(Index, Local) : Spline->InsertPoint(Index, Local);
            });
            if(!bDone) { InConsole.PrintError(QStringLiteral("No point %1").arg(Index)); }
            return;
        }
        if(Verb == QStringLiteral("remove") && InArgs.size() >= 2)
        {
            const int32_t Index = InArgs[1].toInt();
            bool bDone = false;
            RecordEdit(Entity, Type, QStringLiteral("Remove Spline Point"), [&]() { bDone = Spline->RemovePoint(Index); });
            if(!bDone) { InConsole.PrintError(QStringLiteral("No point %1").arg(Index)); }
            return;
        }
        if(Verb == QStringLiteral("clear"))
        {
            RecordEdit(Entity, Type, QStringLiteral("Clear Spline"), [&]() { Spline->ClearPoints(); });
            return;
        }
        uint8_t Flag = 0;
        if(Verb == QStringLiteral("closed") && InArgs.size() >= 2 && ParseFlag(InArgs[1], Flag))
        {
            RecordEdit(Entity, Type, QStringLiteral("Edit Spline"), [&]() { Spline->bClosed = Flag; });
            return;
        }
        if(Verb == QStringLiteral("type") && InArgs.size() >= 2)
        {
            const bool bLinear = InArgs[1].toLower() == QStringLiteral("linear");
            RecordEdit(Entity, Type, QStringLiteral("Edit Spline"), [&]() { Spline->Type = bLinear ? GameCore::ST_Linear : GameCore::ST_Curve; });
            return;
        }
        if(Verb == QStringLiteral("width") && InArgs.size() >= 2)
        {
            const float Width = std::max(InArgs[1].toFloat(), 0.0f);
            RecordEdit(Entity, Type, QStringLiteral("Edit Spline"), [&]() { Spline->Width = Width; });
            return;
        }
        InConsole.PrintError(QStringLiteral("Usage: spline ") + GetSplineUsage());
    }

    // === pcg =====================================================================

    QString GetProceduralUsage()
    {
        return QStringLiteral("<new <name> <x y z> | set <field> <value...> | mesh <i> <mesh|none> [weight] [scale] [far mesh] [material] | "
                              "exclude <i> <entity|none> | regenerate | stats>");
    }

    void RunProceduralCommand(EditorConsole& InConsole, const QStringList& InArgs)
    {
        if(!GHost.Document || InArgs.isEmpty())
        {
            InConsole.PrintError(QStringLiteral("Usage: pcg ") + GetProceduralUsage());
            return;
        }
        const QString Verb = InArgs.front().toLower();
        GameCore::World& World = GHost.Document->GetWorld();

        if(Verb == QStringLiteral("regenerate"))
        {
            RenderCore::RegenerateAllProcedural();
            InConsole.Print(QStringLiteral("Every Procedural Spawner generates again"));
            return;
        }
        if(Verb == QStringLiteral("stats"))
        {
            int32_t Spawners = 0, Total = 0;
            World.Each<RenderCore::ProceduralSpawnerComponent>([&](const GameCore::Entity& InEntity, RenderCore::ProceduralSpawnerComponent& InSpawner)
            {
                ++Spawners;
                Total += InSpawner.InstanceCount;
                InConsole.Print(QStringLiteral("  %1: %2 instances, %3 ms")
                                    .arg(QString::fromUtf8(InEntity.GetName())).arg(InSpawner.InstanceCount)
                                    .arg(double(InSpawner.GenerateMilliseconds), 0, 'f', 1));
            });
            InConsole.Print(QStringLiteral("%1 spawners, %2 instances").arg(Spawners).arg(Total));
            const EngineCore::MemoryPoolStats Pool = RenderCore::GetProceduralMemoryStats();
            InConsole.Print(QStringLiteral("Procedural pool: %1 MB in %2 allocations (peak %3 MB)")
                                .arg(double(Pool.BytesInUse) / (1024.0 * 1024.0), 0, 'f', 2).arg(qulonglong(Pool.LiveAllocations))
                                .arg(double(Pool.PeakBytesInUse) / (1024.0 * 1024.0), 0, 'f', 2));
            return;
        }
        Vector3 Value;
        if(Verb == QStringLiteral("new"))
        {
            if(InArgs.size() < 5 || !ParseVector(InArgs, 2, Value))
            {
                InConsole.PrintError(QStringLiteral("Usage: pcg new <name> <x y z>"));
                return;
            }
            const GameCore::EntityID Made = GHost.Document->CreateEntity(InArgs[1], Value, [](const GameCore::Entity& InEntity)
            {
                InEntity.AddComponent<RenderCore::ProceduralSpawnerComponent>();
            });
            GHost.Document->SetSelection(Made);
            InConsole.Print(QStringLiteral("Made spawner '%1'").arg(InArgs[1]));
            return;
        }

        const GameCore::EntityID Entity = GHost.Document->GetSelection();
        RenderCore::ProceduralSpawnerComponent* Spawner = Entity.IsSet() && World.IsAlive(Entity)
            ? World.GetEntity(Entity).GetComponent<RenderCore::ProceduralSpawnerComponent>() : nullptr;
        if(!Spawner)
        {
            InConsole.PrintError(QStringLiteral("Select an entity with a Procedural Spawner first"));
            return;
        }
        const GameCore::ComponentTypeID Type = RenderCore::ProceduralSpawnerComponent::StaticType().ID;

        if(Verb == QStringLiteral("mesh") && InArgs.size() >= 3)
        {
            const int32_t Index = InArgs[1].toInt();
            if(Index < 0 || Index >= int32_t(RenderCore::PROCEDURAL_MESHES_MAX)) { InConsole.PrintError(QStringLiteral("No entry %1").arg(Index)); return; }
            RecordEdit(Entity, Type, QStringLiteral("Edit Procedural Spawner"), [&]()
            {
                RenderCore::ProceduralMeshEntry& Entry = Spawner->Meshes[Index];
                if(InArgs[2].toLower() == QStringLiteral("none")) { Entry = RenderCore::ProceduralMeshEntry(); return; }
                CopyText(Entry.Mesh, sizeof(Entry.Mesh), InArgs[2]);
                if(InArgs.size() >= 4) { Entry.Weight = InArgs[3].toFloat(); }
                if(InArgs.size() >= 5) { Entry.Scale = InArgs[4].toFloat(); }
                if(InArgs.size() >= 6) { CopyText(Entry.FarMesh, sizeof(Entry.FarMesh), InArgs[5] == QStringLiteral("-") ? QString() : InArgs[5]); }
                if(InArgs.size() >= 7) { CopyText(Entry.Material, sizeof(Entry.Material), InArgs[6] == QStringLiteral("-") ? QString() : InArgs[6]); }
            });
            return;
        }
        if(Verb == QStringLiteral("exclude") && InArgs.size() >= 3)
        {
            const int32_t Index = InArgs[1].toInt();
            if(Index < 0 || Index >= int32_t(RenderCore::PROCEDURAL_EXCLUSIONS_MAX)) { InConsole.PrintError(QStringLiteral("No exclusion %1").arg(Index)); return; }
            const QString Name = InArgs[2].toLower() == QStringLiteral("none") ? QString() : InArgs[2];
            RecordEdit(Entity, Type, QStringLiteral("Edit Procedural Spawner"), [&]()
            {
                CopyText(Spawner->Exclusions[Index], RenderCore::PROCEDURAL_NAME_MAX, Name);
            });
            return;
        }
        if(Verb != QStringLiteral("set") || InArgs.size() < 3)
        {
            InConsole.PrintError(QStringLiteral("Usage: pcg ") + GetProceduralUsage());
            return;
        }

        const QString Field = InArgs[1].toLower();
        const QString Text  = InArgs[2].toLower();
        bool bNumber = false;
        const float Number = InArgs[2].toFloat(&bNumber);
        uint8_t Flag = 0;
        const bool bFlag = ParseFlag(Text, Flag);

        struct FloatField { const char* Name; float RenderCore::ProceduralSpawnerComponent::* Member; };
        static const FloatField Floats[] = {
            { "spacing", &RenderCore::ProceduralSpawnerComponent::Spacing },
            { "jitter", &RenderCore::ProceduralSpawnerComponent::Jitter },
            { "projectrange", &RenderCore::ProceduralSpawnerComponent::ProjectRange },
            { "pathoffset", &RenderCore::ProceduralSpawnerComponent::PathOffset },
            { "density", &RenderCore::ProceduralSpawnerComponent::Density },
            { "noisescale", &RenderCore::ProceduralSpawnerComponent::NoiseScale },
            { "noisethreshold", &RenderCore::ProceduralSpawnerComponent::NoiseThreshold },
            { "maxslope", &RenderCore::ProceduralSpawnerComponent::MaxSlope },
            { "minheight", &RenderCore::ProceduralSpawnerComponent::MinHeight },
            { "maxheight", &RenderCore::ProceduralSpawnerComponent::MaxHeight },
            { "layerthreshold", &RenderCore::ProceduralSpawnerComponent::LayerThreshold },
            { "scalemin", &RenderCore::ProceduralSpawnerComponent::ScaleMin },
            { "scalemax", &RenderCore::ProceduralSpawnerComponent::ScaleMax },
            { "aligntonormal", &RenderCore::ProceduralSpawnerComponent::AlignToNormal },
            { "sink", &RenderCore::ProceduralSpawnerComponent::Sink },
            { "colorvariation", &RenderCore::ProceduralSpawnerComponent::ColorVariation },
            { "cellsize", &RenderCore::ProceduralSpawnerComponent::CellSize },
            { "loddistance", &RenderCore::ProceduralSpawnerComponent::LODDistance },
            { "lod1density", &RenderCore::ProceduralSpawnerComponent::LOD1Density },
            { "lod2density", &RenderCore::ProceduralSpawnerComponent::LOD2Density },
            { "culldistance", &RenderCore::ProceduralSpawnerComponent::CullDistance },
        };
        struct FlagField { const char* Name; uint8_t RenderCore::ProceduralSpawnerComponent::* Member; };
        static const FlagField Flags[] = {
            { "visible", &RenderCore::ProceduralSpawnerComponent::bVisible },
            { "randomyaw", &RenderCore::ProceduralSpawnerComponent::bRandomYaw },
            { "aligntopath", &RenderCore::ProceduralSpawnerComponent::bAlignToPath },
            { "castshadows", &RenderCore::ProceduralSpawnerComponent::bCastShadows },
            { "bothsides", &RenderCore::ProceduralSpawnerComponent::bBothSides },
        };

        bool bKnown = false, bValid = false;
        RecordEdit(Entity, Type, QStringLiteral("Edit Procedural Spawner"), [&]()
        {
            for(const FloatField& Each : Floats)
            {
                if(Field == QLatin1String(Each.Name)) { bKnown = true; bValid = bNumber; if(bNumber) { Spawner->*Each.Member = Number; } return; }
            }
            for(const FlagField& Each : Flags)
            {
                if(Field == QLatin1String(Each.Name)) { bKnown = true; bValid = bFlag; if(bFlag) { Spawner->*Each.Member = Flag; } return; }
            }
            if(Field == QStringLiteral("source"))
            {
                bKnown = true;
                const char* const Names[] = { "path", "area", "box", "surface" };
                for(uint8_t Index = 0; Index < 4; ++Index) { if(Text == QLatin1String(Names[Index])) { Spawner->Source = Index; bValid = true; } }
            }
            else if(Field == QStringLiteral("project"))
            {
                bKnown = true;
                const char* const Names[] = { "none", "terrain", "meshes", "all" };
                for(uint8_t Index = 0; Index < 4; ++Index) { if(Text == QLatin1String(Names[Index])) { Spawner->Project = Index; bValid = true; } }
            }
            else if(Field == QStringLiteral("layer"))
            {
                bKnown = true;
                if(Text == QStringLiteral("any")) { Spawner->TerrainLayer = RenderCore::PROCEDURAL_ANY_LAYER; bValid = true; }
                else if(bNumber && Number >= 0.0f && Number <= 3.0f) { Spawner->TerrainLayer = uint8_t(Number) + 1; bValid = true; }
            }
            else if(Field == QStringLiteral("seed"))      { bKnown = true; bValid = bNumber; if(bNumber) { Spawner->Seed = InArgs[2].toInt(); } }
            else if(Field == QStringLiteral("maxinstances")) { bKnown = true; bValid = bNumber; if(bNumber) { Spawner->MaxInstances = std::max(InArgs[2].toInt(), 1); } }
            else if(Field == QStringLiteral("boxextent"))
            {
                bKnown = true;
                bValid = ParseVector(InArgs, 2, Value);
                if(bValid) { Spawner->BoxExtent = Value; }
            }
        });
        if(!bKnown) { InConsole.PrintError(QStringLiteral("No field '%1'").arg(InArgs[1])); }
        else if(!bValid) { InConsole.PrintError(QStringLiteral("Bad value for %1").arg(InArgs[1])); }
    }
}

#endif // WITH_EDITOR
