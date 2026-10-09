#if WITH_EDITOR

#include "Private/Editor/Terrain/TerrainTool.h"

#include <algorithm>
#include <cmath>

#include <QKeyEvent>
#include <QWheelEvent>

#include "Public/Editor/EditorContext.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/RenderTypes.h"

namespace
{
    constexpr int   RingSegments  = 64;
    constexpr float RadiusMin     = 25.0f;
    constexpr float RadiusMax     = 50000.0f;
    constexpr float RingThickness = 2.0f;

    EngineCore::Color ToolColor(const uint8_t InTool, const bool bInInverted)
    {
        switch(InTool)
        {
            case GameCore::TT_Raise:         return bInInverted ? EngineCore::Color(255, 120, 90) : EngineCore::Color(120, 230, 120);
            case GameCore::TT_Smooth:        return EngineCore::Color(120, 190, 255);
            case GameCore::TT_Flatten:       return EngineCore::Color(255, 210, 90);
            case GameCore::TT_Noise:         return EngineCore::Color(220, 140, 255);
            case GameCore::TT_SetHeight:     return EngineCore::Color(255, 255, 255);
            case GameCore::TT_Paint:         return bInInverted ? EngineCore::Color(120, 190, 255) : EngineCore::Color(255, 160, 60);
            case GameCore::TT_SmoothWeights: return EngineCore::Color(120, 190, 255);
            default:                         return EngineCore::Color(255, 255, 255);
        }
    }
}

GameCore::TerrainBrushOp TerrainTool::MakeOp(const float InDeltaSeconds) const
{
    GameCore::TerrainBrushOp Op;
    Op.Tool         = Current.Tool;
    Op.DeltaSeconds = InDeltaSeconds;
    Op.Rate         = Current.Rate;
    Op.Layer        = Current.Layer;
    Op.NoiseScale   = Current.NoiseScale;
    Op.NoiseSeed    = 1;
    const bool bInvert = bShift != Current.bLower;
    if(Op.Tool == GameCore::TT_Raise && bInvert) { Op.Rate = -Op.Rate; }
    if(Op.Tool == GameCore::TT_Paint && bShift)  { Op.Tool = GameCore::TT_SmoothWeights; }
    if(Op.Tool == GameCore::TT_Flatten)          { Op.TargetHeight = StrokeHeight; }
    if(Op.Tool == GameCore::TT_SetHeight && HitTarget.IsValid())
    {
        Op.TargetHeight = float(HitTarget.ToLocal(EngineCore::Vector3(HitPoint.GetX(), double(Current.TargetHeight), HitPoint.GetZ())).GetY());
    }
    return Op;
}

QString TerrainTool::MakeLabel() const
{
    if(Current.Tool == GameCore::TT_Paint || Current.Tool == GameCore::TT_SmoothWeights)
    {
        if(bShift || Current.Tool == GameCore::TT_SmoothWeights) { return QStringLiteral("Smooth Paint"); }
        const std::string& Name = HitTarget.IsValid() ? HitTarget.Data->LayerNames[std::min<uint32_t>(Current.Layer, 3)] : std::string();
        return QStringLiteral("Paint ") + (Name.empty() ? QStringLiteral("Layer %1").arg(Current.Layer) : QString::fromStdString(Name));
    }
    if(Current.Tool == GameCore::TT_Raise && (bShift != Current.bLower)) { return QStringLiteral("Sculpt Lower"); }
    return QStringLiteral("Sculpt ") + QString::fromLatin1(GameCore::GetTerrainToolName(Current.Tool));
}

bool TerrainTool::Trace(const Ray& InRay)
{
    EditorTerrain::Target Target;
    EngineCore::Vector3   Point;
    bHit = EditorTerrain::PickTerrain(InRay.Origin, InRay.Direction, Target, Point);
    if(bHit && ActiveStroke && Target.Entity != ActiveStroke->GetTarget().Entity)
    {
        bHit = false;   // a stroke stays on the terrain it began on
    }
    if(bHit)
    {
        HitPoint  = Point;
        HitTarget = ActiveStroke ? ActiveStroke->GetTarget() : Target;
    }
    return bHit;
}

bool TerrainTool::OnPress(ViewportWidget& InViewport, const Ray& InRay, const Qt::KeyboardModifiers InModifiers)
{
    (void)InViewport;
    bShift = (InModifiers & Qt::ShiftModifier) != 0;
    if(!Trace(InRay)) { return false;   /* not on a terrain: a click selects, as ever */ }

    const EngineCore::Vector3 Local = HitTarget.ToLocal(HitPoint);
    StrokeHeight = HitTarget.Data->SampleHeight(float(Local.GetX()), float(Local.GetZ()));
    ActiveStroke = std::make_unique<EditorTerrain::Stroke>(HitTarget, MakeLabel());
    return true;
}

void TerrainTool::OnDrag(ViewportWidget& InViewport, const Ray& InRay, const Qt::KeyboardModifiers InModifiers)
{
    (void)InViewport;
    bShift = (InModifiers & Qt::ShiftModifier) != 0;
    Trace(InRay);
}

void TerrainTool::OnRelease(ViewportWidget& InViewport, const bool bInCancel)
{
    (void)InViewport;
    if(!ActiveStroke) { return; }
    ActiveStroke->End(bInCancel);
    ActiveStroke.reset();
}

void TerrainTool::OnHover(ViewportWidget& InViewport, const Ray* InRay, const Qt::KeyboardModifiers InModifiers)
{
    (void)InViewport;
    bShift = (InModifiers & Qt::ShiftModifier) != 0;
    if(!InRay) { bHit = false; return; }
    Trace(*InRay);
}

void TerrainTool::Tick(ViewportWidget& InViewport, const float InDeltaSeconds)
{
    /* The camera may have moved under a still cursor: trace again. */
    Ray Cursor;
    if(InViewport.GetCursorRay(Cursor)) { Trace(Cursor); }
    else if(!ActiveStroke)              { bHit = false; }

    if(!ActiveStroke || !bHit) { return; }
    /* Clamped: a hitch does not dump a second of brush in one frame. */
    const float Delta = std::clamp(InDeltaSeconds, 0.0f, 0.1f);
    GameCore::TerrainBrush Brush;
    Brush.Strength = Current.Strength;
    Brush.Falloff  = Current.Falloff;
    ActiveStroke->Dab(HitPoint, Current.Radius, Brush, MakeOp(Delta));
}

void TerrainTool::QueueOverlay(ViewportWidget& InViewport)
{
    (void)InViewport;
    EditorContext* Context = EditorTerrain::GetHost().Context;
    if(!bHit || !HitTarget.IsValid() || !Context) { return; }

    const EngineCore::Vector3 Centre = HitTarget.ToLocal(HitPoint);
    const float LocalRadius = HitTarget.ToLocalLength(Current.Radius);
    const EngineCore::Color Colour = ToolColor(Current.Tool, bShift != Current.bLower);
    const EngineCore::Color Dim(Colour.R / 2, Colour.G / 2, Colour.B / 2, 255);
    const GameCore::TerrainData& Data = *HitTarget.Data;
    const float Lift = Data.GetSpacing() * 0.05f;

    /* The rim, and where the falloff is half: rings laid on the ground. */
    GameCore::TerrainBrush Probe;
    Probe.Radius  = 1.0f;
    Probe.Falloff = Current.Falloff;
    float Half = 0.5f;
    for(float T = 0.0f; T <= 1.0f; T += 0.02f)
    {
        if(GameCore::GetTerrainBrushWeight(Probe, T) <= 0.5f) { Half = T; break; }
    }
    const float Radii[2] = { LocalRadius, LocalRadius * Half };
    for(int Ring = 0; Ring < 2; ++Ring)
    {
        EngineCore::Vector3 Previous;
        for(int Step = 0; Step <= RingSegments; ++Step)
        {
            const double Angle = double(Step) / double(RingSegments) * 6.283185307179586;
            const float X = float(Centre.GetX() + std::cos(Angle) * Radii[Ring]);
            const float Z = float(Centre.GetZ() + std::sin(Angle) * Radii[Ring]);
            const EngineCore::Vector3 At = HitTarget.ToWorld(EngineCore::Vector3(X, double(Data.SampleHeight(X, Z) + Lift), Z));
            if(Step > 0)
            {
                RenderCore::DebugLine Line;
                Line.Start = Previous;
                Line.End   = At;
                Line.Color = Ring == 0 ? Colour : Dim;
                Line.ThicknessPixels = RingThickness;
                Context->DrawDebugLine(Line);
            }
            Previous = At;
        }
    }

    /* The centre: a short tick up the normal. */
    float Normal[3];
    Data.SampleNormal(float(Centre.GetX()), float(Centre.GetZ()), Normal);
    const float Tick = LocalRadius * 0.15f;
    RenderCore::DebugLine Up;
    Up.Start = HitTarget.ToWorld(EngineCore::Vector3(Centre.GetX(), double(Data.SampleHeight(float(Centre.GetX()), float(Centre.GetZ()))), Centre.GetZ()));
    Up.End   = HitTarget.ToWorld(EngineCore::Vector3(Centre.GetX() + Normal[0] * Tick,
                                                     double(Data.SampleHeight(float(Centre.GetX()), float(Centre.GetZ())) + Normal[1] * Tick),
                                                     Centre.GetZ() + Normal[2] * Tick));
    Up.Color = Colour;
    Up.ThicknessPixels = RingThickness;
    Context->DrawDebugLine(Up);
}

void TerrainTool::Resize(const float InFactor)
{
    Current.Radius = std::clamp(Current.Radius * InFactor, RadiusMin, RadiusMax);
    if(OnSettingsChanged) { OnSettingsChanged(); }
}

bool TerrainTool::OnKey(ViewportWidget& InViewport, QKeyEvent* InEvent)
{
    (void)InViewport;
    switch(InEvent->key())
    {
        case Qt::Key_BracketLeft:  Resize(1.0f / 1.15f); return true;
        case Qt::Key_BracketRight: Resize(1.15f);        return true;
        default:                   return false;
    }
}

bool TerrainTool::OnWheel(ViewportWidget& InViewport, QWheelEvent* InEvent)
{
    (void)InViewport;
    if(!(InEvent->modifiers() & Qt::ControlModifier)) { return false; }
    const double Notches = double(InEvent->angleDelta().y()) / 120.0;
    Resize(float(std::pow(1.15, Notches)));
    return true;
}

#endif // WITH_EDITOR
