#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerCurveEditor.h"

#include <algorithm>
#include <cmath>

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

using Sequencer::ESequenceInterp;
using Sequencer::ESequenceTrackType;
using Sequencer::SequenceTrack;

namespace
{
    constexpr double MARGIN       = 24.0;
    constexpr double KEY_RADIUS   = 5.0;
    constexpr double HANDLE_PIXELS = 46.0;
}

SequencerCurveEditor::SequencerCurveEditor(SequencerDocument& InDocument, QWidget* InParent)
    : QWidget(InParent), Document(InDocument)
{
    setObjectName(QStringLiteral("SequencerCurveEditor"));
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(120);

    const auto Refresh = [this]()
    {
        if(Document.GetCurrentTrack() != ShownTrack && !Document.IsDragging())
        {
            ShownTrack = Document.GetCurrentTrack();
            FitView();
        }
        update();
    };
    connect(&Document, &SequencerDocument::SelectionChanged, this, Refresh);
    connect(&Document, &SequencerDocument::Changed,          this, Refresh);
    connect(&Document, &SequencerDocument::LiveChanged,      this, qOverload<>(&QWidget::update));
    connect(&Document, &SequencerDocument::PlayheadChanged,  this, qOverload<>(&QWidget::update));
}

std::vector<int> SequencerCurveEditor::GetChannels() const
{
    std::vector<int> Channels;
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    if(!Track) { return Channels; }
    if(Track->Type == ESequenceTrackType::Float || Track->Type == ESequenceTrackType::Fade || Track->Type == ESequenceTrackType::Bool
       || Track->Type == ESequenceTrackType::PostProcessSetting || Track->Type == ESequenceTrackType::BusVolume)
    {
        Channels.push_back(0);
        return Channels;
    }
    for(size_t Channel = 0; Channel < Track->Channels.size(); ++Channel)
    {
        if(!Track->Channels[Channel].IsEmpty()) { Channels.push_back(int(Channel)); }
    }
    return Channels;
}

void SequencerCurveEditor::FitView()
{
    FrameMin = 0.0;
    FrameMax = std::max(1.0, double(Document.GetAsset().Length));
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    double Low = 1e30, High = -1e30;
    if(Track)
    {
        for(const int Channel : GetChannels())
        {
            for(double Frame = FrameMin; Frame <= FrameMax; Frame += std::max(1.0, (FrameMax - FrameMin) / 200.0))
            {
                const double V = Track->Channels[size_t(Channel)].Evaluate(Frame, 0.0f);
                Low = std::min(Low, V);
                High = std::max(High, V);
            }
            for(const auto& Key : Track->Channels[size_t(Channel)].Keys)
            {
                Low = std::min(Low, double(Key.Value));
                High = std::max(High, double(Key.Value));
            }
        }
    }
    if(Low > High) { Low = 0.0; High = 1.0; }
    const double Pad = std::max(0.5, (High - Low) * 0.12);
    ValueMin = Low - Pad;
    ValueMax = High + Pad;
    update();
}

QPointF SequencerCurveEditor::ToScreen(const double InFrame, const double InValue) const
{
    const double W = std::max(1.0, width() - 2 * MARGIN), H = std::max(1.0, height() - 2 * MARGIN);
    return QPointF(MARGIN + (InFrame - FrameMin) / (FrameMax - FrameMin) * W,
                   height() - MARGIN - (InValue - ValueMin) / (ValueMax - ValueMin) * H);
}

double SequencerCurveEditor::FrameAt(const double InX) const
{
    return FrameMin + (InX - MARGIN) / std::max(1.0, width() - 2 * MARGIN) * (FrameMax - FrameMin);
}

double SequencerCurveEditor::ValueAt(const double InY) const
{
    return ValueMin + (height() - MARGIN - InY) / std::max(1.0, height() - 2 * MARGIN) * (ValueMax - ValueMin);
}

QColor SequencerCurveEditor::ChannelColor(const int InChannel) const
{
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    if(!Track || Track->Type != ESequenceTrackType::Transform) { return QColor(230, 200, 90); }
    switch(InChannel % 3)
    {
        case 0:  return QColor(230, 90, 90);
        case 1:  return QColor(110, 200, 110);
        default: return QColor(100, 150, 240);
    }
}

QPointF SequencerCurveEditor::HandlePos(const int InChannel, const int InIndex, const bool bInLeaving) const
{
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    const Sequencer::SequenceChannel& Channel = Track->Channels[size_t(InChannel)];
    const Sequencer::SequenceKey& Key = Channel.Keys[size_t(InIndex)];
    const double Slope = bInLeaving ? Channel.GetLeaveSlope(size_t(InIndex)) : Channel.GetArriveSlope(size_t(InIndex));

    /* The slope as a screen direction, a fixed length long. */
    const QPointF At = ToScreen(Key.Frame, Key.Value);
    const QPointF Along = ToScreen(Key.Frame + 1.0, Key.Value + Slope) - At;
    const double Length = std::hypot(Along.x(), Along.y());
    const QPointF Unit = Length > 1e-6 ? Along / Length : QPointF(1, 0);
    return At + Unit * (bInLeaving ? HANDLE_PIXELS : -HANDLE_PIXELS);
}

SequencerCurveEditor::KeyHit SequencerCurveEditor::HitAt(const QPointF& InPos) const
{
    KeyHit Hit;
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    if(!Track) { return Hit; }
    const SequencerTrackRef Ref = Document.GetCurrentTrack();

    /* Handles of selected keys first: they sit over other things. */
    for(const SequencerItem& Item : Document.GetSelection())
    {
        if(Item.Kind != SequencerItem::EKind::Key || Item.Track != Ref) { continue; }
        for(const bool bLeaving : { false, true })
        {
            if(QLineF(HandlePos(Item.Channel, Item.Index, bLeaving), InPos).length() <= KEY_RADIUS + 2)
            {
                return { Item.Channel, Item.Index, bLeaving ? EDrag::Out : EDrag::In };
            }
        }
    }
    for(const int Channel : GetChannels())
    {
        const auto& Keys = Track->Channels[size_t(Channel)].Keys;
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(QLineF(ToScreen(Keys[Index].Frame, Keys[Index].Value), InPos).length() <= KEY_RADIUS + 2)
            {
                return { Channel, int(Index), EDrag::Key };
            }
        }
    }
    return Hit;
}

void SequencerCurveEditor::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    Painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette& Pal = palette();
    Painter.fillRect(rect(), Pal.color(QPalette::Base));

    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    if(!Track || GetChannels().empty())
    {
        Painter.setPen(Pal.color(QPalette::PlaceholderText));
        Painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Pick a track with keys (Transform, a field, Fade or a post process setting) to see its curves"));
        return;
    }

    /* A grid: value lines at round steps, frame lines at seconds. */
    Painter.setPen(Pal.color(QPalette::AlternateBase).darker(110));
    const double Range = ValueMax - ValueMin;
    const double Step  = std::pow(10.0, std::floor(std::log10(Range / 4.0)));
    QFont Small = Painter.font();
    Small.setPointSizeF(std::max(7.0, Small.pointSizeF() - 1.5));
    Painter.setFont(Small);
    /* By index, not by summing: the sum drifts (0 drawn as -2.8e-17). */
    for(double Line = std::ceil(ValueMin / Step); Line * Step <= ValueMax; Line += 1.0)
    {
        const double V = Line * Step;
        const double Y = ToScreen(0, V).y();
        Painter.setPen(Pal.color(QPalette::Mid));
        Painter.drawLine(QPointF(MARGIN, Y), QPointF(width() - MARGIN, Y));
        Painter.setPen(Pal.color(QPalette::PlaceholderText));
        Painter.drawText(QPointF(2, Y - 2), QString::number(V, 'g', 4));
    }
    const uint32_t Fps = std::max<uint32_t>(Document.GetAsset().FrameRate, 1);
    for(double F = 0; F <= FrameMax; F += Fps)
    {
        const double X = ToScreen(F, 0).x();
        Painter.setPen(Pal.color(QPalette::Mid));
        Painter.drawLine(QPointF(X, MARGIN), QPointF(X, height() - MARGIN));
    }

    const SequencerTrackRef Ref = Document.GetCurrentTrack();
    for(const int Channel : GetChannels())
    {
        const Sequencer::SequenceChannel& Keys = Track->Channels[size_t(Channel)];
        const QColor Color = ChannelColor(Channel);
        QPainterPath Curve;
        for(int X = int(MARGIN); X <= width() - int(MARGIN); X += 2)
        {
            const QPointF P = ToScreen(FrameAt(X), Keys.Evaluate(FrameAt(X), 0.0f));
            if(X == int(MARGIN)) { Curve.moveTo(P); } else { Curve.lineTo(P); }
        }
        Painter.setPen(QPen(Color, 1.6));
        Painter.setBrush(Qt::NoBrush);
        Painter.drawPath(Curve);

        for(size_t Index = 0; Index < Keys.Keys.size(); ++Index)
        {
            const auto& Key = Keys.Keys[Index];
            const QPointF At = ToScreen(Key.Frame, Key.Value);
            const bool bSelected = Document.IsSelected({ SequencerItem::EKind::Key, Ref, Channel, int(Index) });
            if(bSelected && Track->Type != ESequenceTrackType::Bool)
            {
                Painter.setPen(QPen(Pal.color(QPalette::Text), 1.0));
                for(const bool bLeaving : { false, true })
                {
                    const QPointF Handle = HandlePos(Channel, int(Index), bLeaving);
                    Painter.drawLine(At, Handle);
                    Painter.setBrush(Pal.color(QPalette::Base));
                    Painter.drawEllipse(Handle, 3.5, 3.5);
                }
            }
            Painter.setPen(QPen(bSelected ? Pal.color(QPalette::Highlight) : QColor(0, 0, 0, 160), bSelected ? 2.5 : 1.0));
            Painter.setBrush(Color);
            Painter.drawEllipse(At, KEY_RADIUS, KEY_RADIUS);
        }
    }

    /* The playhead. */
    const double Head = ToScreen(Document.GetPlayhead(), 0).x();
    Painter.setPen(QPen(QColor(255, 157, 60), 1.5));
    Painter.drawLine(QPointF(Head, MARGIN * 0.5), QPointF(Head, height() - MARGIN * 0.5));
}

void SequencerCurveEditor::mousePressEvent(QMouseEvent* InEvent)
{
    setFocus();
    if(InEvent->button() == Qt::MiddleButton)
    {
        Drag     = EDrag::Pan;
        PressPos = InEvent->position();
        PanFrame[0] = FrameMin; PanFrame[1] = FrameMax;
        PanValue[0] = ValueMin; PanValue[1] = ValueMax;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if(InEvent->button() != Qt::LeftButton) { return; }
    PressPos = InEvent->position();
    bMoved   = false;
    bBroken  = InEvent->modifiers() & Qt::AltModifier;
    const KeyHit Hit = HitAt(PressPos);
    const SequencerTrackRef Ref = Document.GetCurrentTrack();
    if(Hit.Index < 0)
    {
        if(!(InEvent->modifiers() & Qt::ControlModifier)) { Document.ClearSelection(); }
        return;
    }
    const SequencerItem Item{ SequencerItem::EKind::Key, Ref, Hit.Channel, Hit.Index };
    if(Hit.Part == EDrag::Key)
    {
        std::vector<SequencerItem> Selection = Document.GetSelection();
        if(InEvent->modifiers() & Qt::ControlModifier)
        {
            const auto Found = std::find(Selection.begin(), Selection.end(), Item);
            if(Found != Selection.end()) { Selection.erase(Found); } else { Selection.push_back(Item); }
            Document.SetSelection(Selection);
            return;
        }
        if(!Document.IsSelected(Item)) { Document.SetSelection({ Item }); }
    }
    Drag    = Hit.Part;
    Dragged = Hit;
    Document.BeginDrag();
}

void SequencerCurveEditor::mouseMoveEvent(QMouseEvent* InEvent)
{
    if(Drag == EDrag::None) { return; }
    const QPointF Pos = InEvent->position();
    if(Drag == EDrag::Pan)
    {
        const double PerX = (PanFrame[1] - PanFrame[0]) / std::max(1.0, width() - 2 * MARGIN);
        const double PerY = (PanValue[1] - PanValue[0]) / std::max(1.0, height() - 2 * MARGIN);
        const double DF = (Pos.x() - PressPos.x()) * PerX, DV = (Pos.y() - PressPos.y()) * PerY;
        FrameMin = PanFrame[0] - DF; FrameMax = PanFrame[1] - DF;
        ValueMin = PanValue[0] + DV; ValueMax = PanValue[1] + DV;
        update();
        return;
    }
    if(!bMoved && QLineF(Pos, PressPos).length() < 3) { return; }
    bMoved = true;

    Sequencer::SequenceTrack* Track = Document.FindLiveTrack(Document.GetCurrentTrack());
    if(!Track || size_t(Dragged.Channel) >= Track->Channels.size()) { return; }
    auto& Keys = Track->Channels[size_t(Dragged.Channel)].Keys;
    if(size_t(Dragged.Index) >= Keys.size()) { return; }
    Sequencer::SequenceKey& Key = Keys[size_t(Dragged.Index)];

    if(Drag == EDrag::Key)
    {
        Key.Frame = std::max(0, int(std::lround(FrameAt(Pos.x()))));
        Key.Value = Track->Type == ESequenceTrackType::Bool ? (ValueAt(Pos.y()) >= 0.5 ? 1.0f : 0.0f) : float(ValueAt(Pos.y()));
    }
    else
    {
        /* The handle's direction is the slope, in value per frame. */
        const double DFrame = FrameAt(Pos.x()) - double(Key.Frame);
        const double DValue = ValueAt(Pos.y()) - double(Key.Value);
        const bool   bOut   = Drag == EDrag::Out;
        if((bOut && DFrame <= 1e-3) || (!bOut && DFrame >= -1e-3)) { return; }
        const float Slope = float(DValue / DFrame);
        if(Key.Interp != ESequenceInterp::User)
        {
            /* Its present shape kept on the other side. */
            const Sequencer::SequenceChannel Before = Track->Channels[size_t(Dragged.Channel)];
            Key.InTangent  = Before.GetArriveSlope(size_t(Dragged.Index));
            Key.OutTangent = Before.GetLeaveSlope(size_t(Dragged.Index));
            Key.Interp     = ESequenceInterp::User;
        }
        if(bOut || !bBroken) { Key.OutTangent = Slope; }
        if(!bOut || !bBroken) { Key.InTangent = Slope; }
    }
    Document.PreviewDrag();
    update();
}

void SequencerCurveEditor::mouseReleaseEvent(QMouseEvent*)
{
    if(Drag == EDrag::None) { return; }
    const EDrag Was = Drag;
    Drag = EDrag::None;
    if(Was == EDrag::Pan)
    {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if(!bMoved)
    {
        Document.EndDrag(QString(), true);
        return;
    }
    const Sequencer::SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    const int Frame = Track ? Track->Channels[size_t(Dragged.Channel)].Keys[size_t(Dragged.Index)].Frame : 0;
    Document.EndDrag(Was == EDrag::Key ? QStringLiteral("Move Key") : QStringLiteral("Set Tangent"));
    /* The key found again where it went. */
    if(const Sequencer::SequenceTrack* After = Document.FindTrack(Document.GetCurrentTrack()))
    {
        const auto& Keys = After->Channels[size_t(Dragged.Channel)].Keys;
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(Keys[Index].Frame == Frame)
            {
                Document.SetSelection({ { SequencerItem::EKind::Key, Document.GetCurrentTrack(), Dragged.Channel, int(Index) } });
            }
        }
    }
}

void SequencerCurveEditor::mouseDoubleClickEvent(QMouseEvent* InEvent)
{
    const SequenceTrack* Track = Document.FindTrack(Document.GetCurrentTrack());
    if(!Track || HitAt(InEvent->position()).Index >= 0) { return; }
    /* The curve nearest the click, keyed where it was clicked. */
    const double Frame = std::max(0.0, std::round(FrameAt(InEvent->position().x())));
    int    Best = -1;
    double BestDistance = 1e30;
    for(const int Channel : GetChannels())
    {
        const double Y = ToScreen(Frame, Track->Channels[size_t(Channel)].Evaluate(Frame, 0.0f)).y();
        if(std::fabs(Y - InEvent->position().y()) < BestDistance)
        {
            BestDistance = std::fabs(Y - InEvent->position().y());
            Best = Channel;
        }
    }
    if(Best >= 0 && BestDistance < 24.0)
    {
        Document.SetKey(Document.GetCurrentTrack(), Best, int(Frame), Track->Channels[size_t(Best)].Evaluate(Frame, 0.0f),
                        QStringLiteral("Add Key"));
    }
}

void SequencerCurveEditor::wheelEvent(QWheelEvent* InEvent)
{
    const double Factor = std::pow(1.2, -InEvent->angleDelta().y() / 120.0);   // > 1 out
    if(InEvent->modifiers() & Qt::ControlModifier)
    {
        const double Pivot = ValueAt(InEvent->position().y());
        ValueMin = Pivot - (Pivot - ValueMin) * Factor;
        ValueMax = Pivot + (ValueMax - Pivot) * Factor;
    }
    else
    {
        const double Pivot = FrameAt(InEvent->position().x());
        const double NewMin = Pivot - (Pivot - FrameMin) * Factor;
        const double NewMax = Pivot + (FrameMax - Pivot) * Factor;
        if(NewMax - NewMin > 1.0) { FrameMin = NewMin; FrameMax = NewMax; }
    }
    update();
    InEvent->accept();
}

void SequencerCurveEditor::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->key() == Qt::Key_F)
    {
        FitView();
        return;
    }
    if(InEvent->key() == Qt::Key_Delete || InEvent->key() == Qt::Key_Backspace)
    {
        Document.DeleteSelection();
        return;
    }
    QWidget::keyPressEvent(InEvent);
}

void SequencerCurveEditor::contextMenuEvent(QContextMenuEvent* InEvent)
{
    const KeyHit Hit = HitAt(InEvent->pos());
    if(Hit.Index < 0 || Hit.Part != EDrag::Key) { return; }
    const SequencerTrackRef Ref = Document.GetCurrentTrack();
    const SequencerItem Item{ SequencerItem::EKind::Key, Ref, Hit.Channel, Hit.Index };
    if(!Document.IsSelected(Item)) { Document.SetSelection({ Item }); }

    QMenu Menu(this);
    for(size_t Interp = 0; Interp < size_t(ESequenceInterp::Count); ++Interp)
    {
        const ESequenceInterp Each = ESequenceInterp(Interp);
        Menu.addAction(QString::fromLatin1(Sequencer::InterpName(Each)), this, [this, Each]()
        {
            const std::vector<SequencerItem> Keys = Document.GetSelection();
            Document.Edit(QStringLiteral("Set Interpolation"), [&](Sequencer::SequenceAsset&)
            {
                for(const SequencerItem& Key : Keys)
                {
                    if(Key.Kind != SequencerItem::EKind::Key) { continue; }
                    if(Sequencer::SequenceTrack* Track = Document.FindLiveTrack(Key.Track))
                    {
                        Sequencer::SequenceChannel& Channel = Track->Channels[size_t(Key.Channel)];
                        Sequencer::SequenceKey& Target = Channel.Keys[size_t(Key.Index)];
                        if(Each == ESequenceInterp::User && Target.Interp != ESequenceInterp::User)
                        {
                            Target.InTangent  = Channel.GetArriveSlope(size_t(Key.Index));
                            Target.OutTangent = Channel.GetLeaveSlope(size_t(Key.Index));
                        }
                        Target.Interp = Each;
                    }
                }
            });
        });
    }
    Menu.exec(InEvent->globalPos());
}

void SequencerCurveEditor::resizeEvent(QResizeEvent* InEvent)
{
    QWidget::resizeEvent(InEvent);
    update();
}

#endif // WITH_EDITOR
