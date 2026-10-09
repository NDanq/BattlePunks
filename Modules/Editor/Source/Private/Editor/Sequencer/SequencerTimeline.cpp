#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerTimeline.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QContextMenuEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#if WITH_AUDIO
#include "Public/Audio/SoundAsset.h"
#endif

using Sequencer::ESequenceTrackType;
using Sequencer::SequenceTrack;

namespace
{
    constexpr int KEY_HIT   = 7;   // pixels either side of a key
    constexpr int EDGE_HIT  = 6;   // pixels either side of a section's edge
    constexpr int ARROW_W   = 18;
    constexpr int INDENT    = 20;   // per depth

    QString TrackLabel(const SequenceTrack& InTrack)
    {
        QString Label = !InTrack.Name.empty() ? QString::fromStdString(InTrack.Name) : QString::fromLatin1(Sequencer::TrackTypeName(InTrack.Type));
        if((InTrack.Type == ESequenceTrackType::Float || InTrack.Type == ESequenceTrackType::Bool) && InTrack.Name.empty())
        {
            Label = QStringLiteral("%1.%2").arg(QString::fromStdString(InTrack.Component), QString::fromStdString(InTrack.Field));
        }
        else if(InTrack.Type == ESequenceTrackType::PostProcess && InTrack.Name.empty())
        {
            Label = QStringLiteral("Post Process");
        }
        else if(InTrack.Type == ESequenceTrackType::PostProcessSetting && InTrack.Name.empty())
        {
            Label = QStringLiteral("Post Process: %1").arg(QString::fromStdString(InTrack.Field));
        }
        else if(InTrack.Type == ESequenceTrackType::BusVolume && InTrack.Name.empty())
        {
            Label = QStringLiteral("Bus Volume: %1").arg(QString::fromStdString(InTrack.Field));
        }
        else if(InTrack.Type == ESequenceTrackType::Subtitle && InTrack.Name.empty())
        {
            Label = QStringLiteral("Subtitles");
        }
        return Label;
    }

    /* A subtitle section's caption: "Speaker: line". */
    QString SubtitleCaption(const Sequencer::SequenceSection& InSection)
    {
        const QString Text = QString::fromStdString(InSection.Asset).simplified();
        return InSection.Speaker.empty() ? Text : QStringLiteral("%1: %2").arg(QString::fromStdString(InSection.Speaker), Text);
    }

#if WITH_AUDIO
    /* An Audio section's sound, its peaks across the box: where in the sound
     * each pixel plays (its offset and play rate; wrapping if the sound
     * loops). Nothing for a sound that does not load. */
    void DrawWaveform(QPainter& InPainter, const QRectF& InBox, const Sequencer::SequenceSection& InSection, const double InPixelsPerFrame,
                      const uint32_t InFps, const QColor& InColor)
    {
        if(InSection.Asset.empty()) { return; }
        const std::shared_ptr<const Audio::SoundData> Sound = Audio::SoundAssetCache::Find(InSection.Asset);
        if(!Sound || Sound->Cooked.Waveform.empty()) { return; }
        const double Duration = Sound->Cooked.GetDuration();
        if(Duration <= 0.0) { return; }
        const size_t Buckets = Sound->Cooked.Waveform.size();
        const double Mid  = InBox.center().y();
        const double Half = InBox.height() * 0.5 - 3.0;
        const QRectF Visible = InBox.intersected(InPainter.clipBoundingRect().isEmpty() ? InBox : InPainter.clipBoundingRect());
        InPainter.setPen(QPen(InColor, 1.0));
        for(int X = int(std::floor(Visible.left())); X < int(std::ceil(Visible.right())); ++X)
        {
            const double Frames = (X - InBox.left()) / InPixelsPerFrame + InSection.Offset;
            double Seconds = Frames / double(InFps) * InSection.PlayRate;
            if(Sound->Settings.bLoop) { Seconds = std::fmod(Seconds, Duration); }
            if(Seconds < 0.0 || Seconds >= Duration) { continue; }
            const size_t Bucket = std::min(Buckets - 1, size_t(Seconds / Duration * double(Buckets)));
            const double Peak = std::max(0.5, Sound->Cooked.Waveform[Bucket] / 255.0 * Half);
            InPainter.drawLine(QPointF(X + 0.5, Mid - Peak), QPointF(X + 0.5, Mid + Peak));
        }
    }
#endif

    void Diamond(QPainter& InPainter, const QPointF& InAt, const double InHalf, const QColor& InFill, const bool bInSelected,
                 const QColor& InSelectedRing)
    {
        QPainterPath Path;
        Path.moveTo(InAt.x(), InAt.y() - InHalf);
        Path.lineTo(InAt.x() + InHalf, InAt.y());
        Path.lineTo(InAt.x(), InAt.y() + InHalf);
        Path.lineTo(InAt.x() - InHalf, InAt.y());
        Path.closeSubpath();
        InPainter.setPen(QPen(bInSelected ? InSelectedRing : QColor(0, 0, 0, 160), bInSelected ? 2.0 : 1.0));
        InPainter.setBrush(bInSelected ? InFill.lighter(130) : InFill);
        InPainter.drawPath(Path);
    }
}

SequencerTimeline::SequencerTimeline(SequencerDocument& InDocument, QWidget* InParent)
    : QWidget(InParent), Document(InDocument)
{
    setObjectName(QStringLiteral("SequencerTimeline"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(RULER_HEIGHT + TRACK_HEIGHT * 4);

    connect(&Document, &SequencerDocument::RowsChanged,      this, [this]() { RebuildRows(); update(); });
    connect(&Document, &SequencerDocument::Changed,          this, [this]() { RebuildRows(); update(); });
    connect(&Document, &SequencerDocument::LiveChanged,      this, qOverload<>(&QWidget::update));
    connect(&Document, &SequencerDocument::SelectionChanged, this, qOverload<>(&QWidget::update));
    connect(&Document, &SequencerDocument::PlayheadChanged,  this, qOverload<>(&QWidget::update));
    RebuildRows();
}

// === Geometry ====================================================================

void SequencerTimeline::RebuildRows()
{
    Rows.clear();
    const Sequencer::SequenceAsset& Asset = Document.GetAsset();
    for(size_t Binding = 0; Binding < Asset.Bindings.size(); ++Binding)
    {
        const Sequencer::SequenceBinding& Each = Asset.Bindings[Binding];
        Row Head;
        Head.Kind        = Row::EKind::Binding;
        Head.Binding     = int(Binding);
        Head.CollapseKey = QStringLiteral("b:%1").arg(QString::fromStdString(Each.Id));
        Head.Label       = QString::fromStdString(Each.Name.empty() ? Each.Id : Each.Name);
        if(Each.Kind == Sequencer::ESequenceBindingKind::Spawn) { Head.Label += QStringLiteral("  (spawned)"); }
        Rows.push_back(Head);
        if(Document.IsCollapsed(Head.CollapseKey)) { continue; }

        for(size_t Track = 0; Track < Each.Tracks.size(); ++Track)
        {
            const SequenceTrack& T = Each.Tracks[Track];
            Row TrackRow;
            TrackRow.Kind    = Row::EKind::Track;
            TrackRow.Binding = int(Binding);
            TrackRow.Track   = { int(Binding), int(Track) };
            TrackRow.Depth   = 1;
            TrackRow.Label   = TrackLabel(T);
            if(T.Type == ESequenceTrackType::Transform)
            {
                TrackRow.CollapseKey = QStringLiteral("t:%1:%2").arg(QString::fromStdString(Each.Id)).arg(Track);
            }
            Rows.push_back(TrackRow);
            if(T.Type != ESequenceTrackType::Transform || Document.IsCollapsed(TrackRow.CollapseKey)) { continue; }
            for(size_t Channel = 0; Channel < T.Channels.size(); ++Channel)
            {
                if(T.Channels[Channel].IsEmpty()) { continue; }
                Row ChannelRow;
                ChannelRow.Kind    = Row::EKind::Channel;
                ChannelRow.Binding = int(Binding);
                ChannelRow.Track   = TrackRow.Track;
                ChannelRow.Channel = int(Channel);
                ChannelRow.Depth   = 2;
                ChannelRow.Label   = QString::fromLatin1(Sequencer::TransformChannelName(Channel));
                Rows.push_back(ChannelRow);
            }
        }
    }

    Row Header;
    Header.Kind  = Row::EKind::Header;
    Header.Label = QStringLiteral("Sequence");
    Rows.push_back(Header);
    for(size_t Track = 0; Track < Asset.Tracks.size(); ++Track)
    {
        Row TrackRow;
        TrackRow.Kind  = Row::EKind::Track;
        TrackRow.Track = { -1, int(Track) };
        TrackRow.Depth = 1;
        TrackRow.Label = TrackLabel(Asset.Tracks[Track]);
        Rows.push_back(TrackRow);
    }
    ScrollRow = std::clamp(ScrollRow, 0, std::max(0, int(Rows.size()) - 1));
}

int SequencerTimeline::RowHeight(const int InRow) const
{
    if(InRow < 0 || size_t(InRow) >= Rows.size()) { return TRACK_HEIGHT; }
    const Row& R = Rows[size_t(InRow)];
    switch(R.Kind)
    {
        case Row::EKind::Binding: return BINDING_HEIGHT;
        case Row::EKind::Header:  return BINDING_HEIGHT;
        case Row::EKind::Channel: return CHANNEL_HEIGHT;
        case Row::EKind::Track:   break;
    }
    const SequenceTrack* Track = Document.FindTrack(R.Track);
    return Track && (Sequencer::IsSectionTrack(Track->Type) || Track->Type == ESequenceTrackType::Event) ? SECTION_HEIGHT : TRACK_HEIGHT;
}

int SequencerTimeline::RowAt(const int InY) const
{
    if(InY < RULER_HEIGHT) { return -1; }
    int Top = RULER_HEIGHT;
    for(int Row = ScrollRow; Row < int(Rows.size()); ++Row)
    {
        const int Height = RowHeight(Row);
        if(InY < Top + Height) { return Row; }
        Top += Height;
    }
    return -1;
}

void SequencerTimeline::ScrollToTrack(const SequencerTrackRef& InTrack)
{
    for(int Index = 0; Index < int(Rows.size()); ++Index)
    {
        if(Rows[size_t(Index)].Kind != Row::EKind::Track || Rows[size_t(Index)].Track != InTrack) { continue; }
        const QRect Rect = RowRect(Index);
        if(Index < ScrollRow || Rect.bottom() > height())
        {
            ScrollRow = std::max(0, Index - 1);
            update();
        }
        return;
    }
}

QRect SequencerTimeline::RowRect(const int InRow) const
{
    int Top = RULER_HEIGHT;
    for(int Row = ScrollRow; Row < InRow; ++Row) { Top += RowHeight(Row); }
    for(int Row = InRow; Row < ScrollRow; ++Row) { Top -= RowHeight(Row); }
    return QRect(0, Top, width(), RowHeight(InRow));
}

double SequencerTimeline::FrameAtX(const double InX) const
{
    return ScrollFrame + (InX - double(NAME_WIDTH)) / PixelsPerFrame;
}

double SequencerTimeline::XOfFrame(const double InFrame) const
{
    return double(NAME_WIDTH) + (InFrame - ScrollFrame) * PixelsPerFrame;
}

void SequencerTimeline::SetPixelsPerFrame(const double InPixels)
{
    const double Clamped = std::clamp(InPixels, MIN_PIXELS_PER_FRAME, MAX_PIXELS_PER_FRAME);
    if(Clamped == PixelsPerFrame) { return; }
    PixelsPerFrame = Clamped;
    update();
    emit ViewChanged();
}

void SequencerTimeline::SetScrollFrame(const double InFrame)
{
    /* Not far past either end: a little margin, as FitAll leaves. */
    const double Length = double(std::max(Document.GetAsset().Length, 1));
    const double Clamped = std::clamp(InFrame, -GetVisibleFrames() * 0.5, Length);
    if(Clamped == ScrollFrame) { return; }
    ScrollFrame = Clamped;
    update();
    emit ViewChanged();
}

double SequencerTimeline::GetVisibleFrames() const
{
    return double(std::max(1, width() - NAME_WIDTH)) / PixelsPerFrame;
}

void SequencerTimeline::ZoomBy(const double InFactor, double InPivotX)
{
    if(InPivotX < NAME_WIDTH)
    {
        const double Head = XOfFrame(Document.GetPlayhead());
        InPivotX = Head >= NAME_WIDTH && Head <= width() ? Head : (NAME_WIDTH + width()) * 0.5;
    }
    const double Pivot = FrameAtX(InPivotX);
    SetPixelsPerFrame(PixelsPerFrame * InFactor);
    SetScrollFrame(Pivot - (InPivotX - NAME_WIDTH) / PixelsPerFrame);
}

void SequencerTimeline::FitAll()
{
    /* Not laid out yet (a sequence opened into a hidden dock): fit once it is. */
    if(!isVisible() || width() <= NAME_WIDTH + 100)
    {
        bFitPending = true;
        return;
    }
    bFitPending = false;
    /* A margin either side, so keys on the first and last frames show whole. */
    const double Lane = std::max(50, width() - NAME_WIDTH - 48);
    SetPixelsPerFrame(Lane / double(std::max(Document.GetAsset().Length, 1)));
    ScrollFrame = -16.0 / PixelsPerFrame;
    update();
    emit ViewChanged();
}

QColor SequencerTimeline::TrackColor(const ESequenceTrackType InType) const
{
    switch(InType)
    {
        case ESequenceTrackType::Transform:   return QColor(77, 143, 216);
        case ESequenceTrackType::Float:       return QColor(77, 143, 216);
        case ESequenceTrackType::Bool:        return QColor(74, 166, 166);
        case ESequenceTrackType::Event:       return QColor(212, 86, 107);
        case ESequenceTrackType::Animation:   return QColor(63, 174, 122);
        case ESequenceTrackType::Audio:       return QColor(143, 106, 214);
        case ESequenceTrackType::CameraCut:   return QColor(201, 138, 43);
        case ESequenceTrackType::Fade:        return QColor(140, 148, 162);
        case ESequenceTrackType::PostProcess: return QColor(196, 104, 176);
        case ESequenceTrackType::PostProcessSetting: return QColor(196, 104, 176);
        case ESequenceTrackType::BusVolume:   return QColor(118, 92, 184);
        case ESequenceTrackType::Subtitle:    return QColor(186, 160, 74);
        case ESequenceTrackType::SubSequence: return QColor(180, 120, 80);
        case ESequenceTrackType::Count:       break;
    }
    return QColor(128, 128, 128);
}

// === Painting ====================================================================

void SequencerTimeline::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    Painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette& Pal = palette();
    Painter.fillRect(rect(), Pal.color(QPalette::Base));

    for(int Row = ScrollRow; Row < int(Rows.size()); ++Row)
    {
        const QRect Rect = RowRect(Row);
        if(Rect.top() > height()) { break; }
        PaintRow(Painter, Row, Rect);
    }

    /* Past the end of the sequence, shaded. */
    const double End = XOfFrame(double(Document.GetAsset().Length));
    if(End < width())
    {
        Painter.fillRect(QRectF(std::max(End, double(NAME_WIDTH)), RULER_HEIGHT, width() - End, height()), QColor(0, 0, 0, 60));
    }

    PaintRuler(Painter, QRect(0, 0, width(), RULER_HEIGHT));

    /* The playhead, over everything but the names. */
    const double Head = XOfFrame(Document.GetPlayhead());
    if(Head >= NAME_WIDTH)
    {
        Painter.setPen(QPen(QColor(255, 157, 60), 2.0));
        Painter.drawLine(QPointF(Head, 0), QPointF(Head, height()));
        QPainterPath Cap;
        Cap.moveTo(Head - 6, 0);
        Cap.lineTo(Head + 6, 0);
        Cap.lineTo(Head, 9);
        Cap.closeSubpath();
        Painter.fillPath(Cap, QColor(255, 157, 60));
    }

    if(Drag == EDrag::Box)
    {
        Painter.setPen(QPen(Pal.color(QPalette::Highlight), 1.0, Qt::DashLine));
        Painter.setBrush(QColor(Pal.color(QPalette::Highlight).red(), Pal.color(QPalette::Highlight).green(),
                                Pal.color(QPalette::Highlight).blue(), 40));
        Painter.drawRect(QRect(PressPos, DragPos).normalized());
    }

    /* The name column's edge. */
    Painter.setPen(Pal.color(QPalette::Mid));
    Painter.drawLine(NAME_WIDTH, 0, NAME_WIDTH, height());
}

void SequencerTimeline::PaintRuler(QPainter& InPainter, const QRect& InRect)
{
    const QPalette& Pal = palette();
    InPainter.fillRect(InRect, Pal.color(QPalette::Window).darker(115));
    InPainter.setPen(Pal.color(QPalette::Mid));
    InPainter.drawLine(InRect.bottomLeft(), InRect.bottomRight());

    const Sequencer::SequenceAsset& Asset = Document.GetAsset();
    InPainter.setPen(Pal.color(QPalette::PlaceholderText));
    InPainter.drawText(QRect(8, 0, NAME_WIDTH - 16, RULER_HEIGHT), Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("%1 fps  ·  %2 frames").arg(Asset.FrameRate).arg(Asset.Length));

    /* Major ticks a whole number of frames under a second, or whole seconds
     * (and minutes) above: labels where they fit, in seconds:frames. */
    const int Fps = int(std::max<uint32_t>(Asset.FrameRate, 1));
    std::vector<int> Steps = { 1, 2, 5, 10 };
    for(const int Divisor : { 4, 2 }) { if(Fps % Divisor == 0 && Fps / Divisor > 10) { Steps.push_back(Fps / Divisor); } }
    for(const int Seconds : { 1, 2, 5, 10, 15, 30, 60, 120, 300 }) { Steps.push_back(Fps * Seconds); }
    std::sort(Steps.begin(), Steps.end());
    int Major = Steps.back();
    for(const int Step : Steps)
    {
        if(Step * PixelsPerFrame >= 96.0) { Major = Step; break; }
    }
    const int Minor = std::max(1, Major / 5);
    const int First = int(std::floor(std::max(0.0, ScrollFrame) / Minor)) * Minor;
    InPainter.save();
    InPainter.setClipRect(QRect(NAME_WIDTH, 0, width() - NAME_WIDTH, RULER_HEIGHT));
    for(int Frame = First; XOfFrame(Frame) < width(); Frame += Minor)
    {
        const double X = XOfFrame(Frame);
        const bool bMajor = Frame % Major == 0;
        InPainter.setPen(Pal.color(bMajor ? QPalette::Text : QPalette::Mid));
        InPainter.drawLine(QPointF(X, RULER_HEIGHT - (bMajor ? 14 : 6)), QPointF(X, RULER_HEIGHT));
        if(bMajor)
        {
            const uint32_t Fps = std::max<uint32_t>(Asset.FrameRate, 1);
            InPainter.drawText(QPointF(X + 4, 16), QStringLiteral("%1:%2").arg(Frame / int(Fps)).arg(Frame % int(Fps), 2, 10, QLatin1Char('0')));
        }
    }

    /* Markers: flags along the ruler's foot. */
    for(size_t Index = 0; Index < Asset.Markers.size(); ++Index)
    {
        const double X = XOfFrame(Asset.Markers[Index].Frame);
        const bool bSelected = Document.IsSelected({ SequencerItem::EKind::Marker, {}, 0, int(Index) });
        QPainterPath Flag;
        Flag.moveTo(X, RULER_HEIGHT - 16);
        Flag.lineTo(X + 10, RULER_HEIGHT - 11);
        Flag.lineTo(X, RULER_HEIGHT - 6);
        Flag.closeSubpath();
        InPainter.fillPath(Flag, bSelected ? Pal.color(QPalette::Highlight) : QColor(106, 168, 255));
        InPainter.setPen(QColor(106, 168, 255));
        InPainter.drawText(QPointF(X + 13, RULER_HEIGHT - 6), QString::fromStdString(Asset.Markers[Index].Name));
    }
    InPainter.restore();
}

void SequencerTimeline::PaintRow(QPainter& InPainter, const int InRow, const QRect& InRect)
{
    const QPalette& Pal = palette();
    const Row& R = Rows[size_t(InRow)];
    const SequenceTrack* Track = R.Track.IsValid() ? Document.FindTrack(R.Track) : nullptr;

    /* The lane. */
    const QRect Lane(NAME_WIDTH, InRect.top(), InRect.width() - NAME_WIDTH, InRect.height());
    QColor LaneColor = Pal.color(QPalette::Base);
    if(R.Kind == Row::EKind::Binding || R.Kind == Row::EKind::Header) { LaneColor = Pal.color(QPalette::AlternateBase); }
    InPainter.fillRect(Lane, LaneColor);
    InPainter.setPen(Pal.color(QPalette::Window));
    InPainter.drawLine(InRect.bottomLeft(), InRect.bottomRight());

    /* The name. */
    const QRect Name(0, InRect.top(), NAME_WIDTH, InRect.height());
    const bool bCurrent = (R.Kind == Row::EKind::Binding && R.Binding == Document.GetCurrentBinding() && !Document.GetCurrentTrack().IsValid())
                       || (R.Kind != Row::EKind::Binding && R.Track.IsValid() && R.Track == Document.GetCurrentTrack() && R.Channel < 0);
    InPainter.fillRect(Name, bCurrent ? Pal.color(QPalette::Highlight).darker(130) : Pal.color(QPalette::Window));
    const int Indent = 8 + R.Depth * INDENT;
    if(!R.CollapseKey.isEmpty())
    {
        const bool bCollapsed = Document.IsCollapsed(R.CollapseKey);
        InPainter.setPen(Pal.color(QPalette::Text));
        InPainter.drawText(QRect(Indent, InRect.top(), ARROW_W, InRect.height()), Qt::AlignCenter,
                           bCollapsed ? QStringLiteral("▸") : QStringLiteral("▾"));
    }
    int TextX = Indent + ARROW_W;
    if(Track)
    {
        InPainter.setBrush(TrackColor(Track->Type));
        InPainter.setPen(Qt::NoPen);
        InPainter.drawRoundedRect(QRectF(TextX, InRect.center().y() - 5, 10, 10), 2.5, 2.5);
        TextX += 17;
    }
    QFont Font = InPainter.font();
    Font.setBold(R.Kind == Row::EKind::Binding || R.Kind == Row::EKind::Header);
    InPainter.setFont(Font);
    InPainter.setPen(Track && Track->bMuted ? Pal.color(QPalette::PlaceholderText) : Pal.color(QPalette::Text));
    QString Label = R.Label;
    if(Track && Track->bMuted && R.Kind == Row::EKind::Track) { Label += QStringLiteral("  (muted)"); }
    InPainter.drawText(QRect(TextX, InRect.top(), NAME_WIDTH - TextX - 8, InRect.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       InPainter.fontMetrics().elidedText(Label, Qt::ElideRight, NAME_WIDTH - TextX - 8));
    Font.setBold(false);
    InPainter.setFont(Font);

    if(!Track) { return; }

    InPainter.save();
    InPainter.setClipRect(Lane);
    const QColor Color = TrackColor(Track->Type);
    const double Mid = InRect.top() + InRect.height() * 0.5;

    /* Sections. */
    for(size_t Index = 0; Index < Track->Sections.size(); ++Index)
    {
        const Sequencer::SequenceSection& Section = Track->Sections[Index];
        const QRectF Box(XOfFrame(Section.Start), InRect.top() + 5, std::max(2.0, Section.GetLength() * PixelsPerFrame), InRect.height() - 10);
        const bool bSelected = Document.IsSelected({ SequencerItem::EKind::Section, R.Track, 0, int(Index) });
        QColor Fill = Color;
        Fill.setAlpha(Track->bMuted ? 90 : 210);
        InPainter.setPen(QPen(bSelected ? Pal.color(QPalette::Highlight) : QColor(255, 255, 255, 40), bSelected ? 2.5 : 1.0));
        InPainter.setBrush(Fill);
        InPainter.drawRoundedRect(Box, 4, 4);
        if(Section.BlendIn > 0)
        {
            QPainterPath Ramp;
            Ramp.moveTo(Box.left(), Box.bottom());
            Ramp.lineTo(Box.left() + Section.BlendIn * PixelsPerFrame, Box.top());
            Ramp.lineTo(Box.left(), Box.top());
            Ramp.closeSubpath();
            InPainter.fillPath(Ramp, QColor(0, 0, 0, 90));
        }
        if(Section.BlendOut > 0)
        {
            QPainterPath Ramp;
            Ramp.moveTo(Box.right(), Box.bottom());
            Ramp.lineTo(Box.right() - Section.BlendOut * PixelsPerFrame, Box.top());
            Ramp.lineTo(Box.right(), Box.top());
            Ramp.closeSubpath();
            InPainter.fillPath(Ramp, QColor(0, 0, 0, 90));
        }
#if WITH_AUDIO
        if(Track->Type == ESequenceTrackType::Audio)
        {
            InPainter.save();
            InPainter.setClipRect(Box.adjusted(1, 1, -1, -1), Qt::IntersectClip);
            DrawWaveform(InPainter, Box, Section, PixelsPerFrame, std::max<uint32_t>(Document.GetAsset().FrameRate, 1), QColor(255, 255, 255, 100));
            InPainter.restore();
        }
#endif
        QString Text = QString::fromStdString(Section.Asset);
        if(Track->Type == ESequenceTrackType::Subtitle) { Text = SubtitleCaption(Section); }
        else if(Track->Type != ESequenceTrackType::CameraCut) { Text = QFileInfo(Text).completeBaseName(); }
        else if(const int Bound = Document.GetAsset().FindBinding(Section.Asset); Bound >= 0)
        {
            const auto& Binding = Document.GetAsset().Bindings[size_t(Bound)];
            Text = QString::fromStdString(Binding.Name.empty() ? Binding.Id : Binding.Name);
        }
        if(Text.isEmpty()) { Text = QStringLiteral("(nothing)"); }
        InPainter.setPen(QColor(255, 255, 255));
        InPainter.drawText(Box.adjusted(9, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           InPainter.fontMetrics().elidedText(Text, Qt::ElideRight, int(Box.width()) - 15));
    }

    /* Events: flags with their names. */
    for(size_t Index = 0; Index < Track->Events.size(); ++Index)
    {
        const Sequencer::SequenceEventKey& Event = Track->Events[Index];
        const double X = XOfFrame(Event.Frame);
        const bool bSelected = Document.IsSelected({ SequencerItem::EKind::Event, R.Track, 0, int(Index) });
        const QColor Flag = bSelected ? Pal.color(QPalette::Highlight) : Color;
        InPainter.setPen(QPen(Flag, 2.0));
        InPainter.drawLine(QPointF(X, InRect.top() + 5), QPointF(X, InRect.bottom() - 5));
        InPainter.fillRect(QRectF(X, InRect.top() + 5, 11, 9), Flag);
        InPainter.setPen(Pal.color(QPalette::Text));
        InPainter.drawText(QRectF(X + 15, InRect.top(), 400, InRect.height()), Qt::AlignVCenter | Qt::AlignLeft,
                           QString::fromStdString(Event.Event));
    }

    /* Keys: a channel's own, a one-channel track's, or a Transform track's
     * summary (every channel's frames, smaller). */
    if(R.Kind == Row::EKind::Channel || (Track->Type != ESequenceTrackType::Transform && !Track->Channels.empty()))
    {
        const int Channel = R.Kind == Row::EKind::Channel ? R.Channel : 0;
        const auto& Keys = Track->Channels[size_t(Channel)].Keys;

        /* The curve's shape, faintly, behind its keys. */
        if(Track->Type == ESequenceTrackType::Fade || Track->Type == ESequenceTrackType::Float || Track->Type == ESequenceTrackType::Bool
           || Track->Type == ESequenceTrackType::PostProcessSetting || Track->Type == ESequenceTrackType::BusVolume)
        {
            float Low = 0.0f, High = 1.0f;
            for(const auto& Key : Keys) { Low = std::min(Low, Key.Value); High = std::max(High, Key.Value); }
            const double Span = std::max(1e-3f, High - Low);
            QPainterPath Curve;
            for(int X = NAME_WIDTH; X < width(); X += 3)
            {
                const double V = Track->Channels[size_t(Channel)].Evaluate(FrameAtX(X), 0.0f);
                const double Y = InRect.bottom() - 6 - (V - Low) / Span * (InRect.height() - 12);
                if(X == NAME_WIDTH) { Curve.moveTo(X, Y); } else { Curve.lineTo(X, Y); }
            }
            InPainter.setPen(QPen(QColor(Color.red(), Color.green(), Color.blue(), 110), 1.0));
            InPainter.setBrush(Qt::NoBrush);
            InPainter.drawPath(Curve);
        }
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            const bool bSelected = Document.IsSelected({ SequencerItem::EKind::Key, R.Track, Channel, int(Index) });
            Diamond(InPainter, QPointF(XOfFrame(Keys[Index].Frame), Mid), bSelected ? 7.5 : 6.0, Color, bSelected, Pal.color(QPalette::Highlight));
        }
    }
    else if(Track->Type == ESequenceTrackType::Transform)
    {
        std::set<int> Frames;
        for(const auto& Channel : Track->Channels)
        {
            for(const auto& Key : Channel.Keys) { Frames.insert(Key.Frame); }
        }
        for(const int Frame : Frames)
        {
            bool bSelected = false;
            for(size_t Channel = 0; Channel < Track->Channels.size() && !bSelected; ++Channel)
            {
                const auto& Keys = Track->Channels[Channel].Keys;
                for(size_t Index = 0; Index < Keys.size(); ++Index)
                {
                    if(Keys[Index].Frame == Frame && Document.IsSelected({ SequencerItem::EKind::Key, R.Track, int(Channel), int(Index) }))
                    {
                        bSelected = true;
                    }
                }
            }
            Diamond(InPainter, QPointF(XOfFrame(Frame), Mid), bSelected ? 6.0 : 4.5, QColor(170, 177, 191), bSelected, Pal.color(QPalette::Highlight));
        }
    }
    InPainter.restore();
}

// === Hit testing =================================================================

SequencerItem SequencerTimeline::HitItem(const QPoint& InPos, bool* bOutEdgeStart, bool* bOutEdgeEnd) const
{
    if(bOutEdgeStart) { *bOutEdgeStart = false; }
    if(bOutEdgeEnd)   { *bOutEdgeEnd = false; }
    SequencerItem None;
    if(InPos.x() < NAME_WIDTH) { return None; }

    if(InPos.y() < RULER_HEIGHT)
    {
        const auto& Markers = Document.GetAsset().Markers;
        for(size_t Index = 0; Index < Markers.size(); ++Index)
        {
            if(std::fabs(XOfFrame(Markers[Index].Frame) - InPos.x()) <= KEY_HIT + 4 && InPos.y() >= RULER_HEIGHT - 18)
            {
                return { SequencerItem::EKind::Marker, {}, 0, int(Index) };
            }
        }
        return None;
    }

    const int RowIndex = RowAt(InPos.y());
    if(RowIndex < 0) { return None; }
    const Row& R = Rows[size_t(RowIndex)];
    const SequenceTrack* Track = R.Track.IsValid() ? Document.FindTrack(R.Track) : nullptr;
    if(!Track) { return None; }

    const auto NearKey = [&](const int InFrame) { return std::fabs(XOfFrame(InFrame) - InPos.x()) <= KEY_HIT; };

    if(R.Kind == Row::EKind::Channel || (Track->Type != ESequenceTrackType::Transform && !Track->Channels.empty()))
    {
        const int Channel = R.Kind == Row::EKind::Channel ? R.Channel : 0;
        const auto& Keys = Track->Channels[size_t(Channel)].Keys;
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(NearKey(Keys[Index].Frame)) { return { SequencerItem::EKind::Key, R.Track, Channel, int(Index) }; }
        }
    }
    else if(Track->Type == ESequenceTrackType::Transform)
    {
        for(size_t Channel = 0; Channel < Track->Channels.size(); ++Channel)
        {
            const auto& Keys = Track->Channels[Channel].Keys;
            for(size_t Index = 0; Index < Keys.size(); ++Index)
            {
                if(NearKey(Keys[Index].Frame)) { return { SequencerItem::EKind::Key, R.Track, int(Channel), int(Index) }; }
            }
        }
    }
    for(size_t Index = 0; Index < Track->Events.size(); ++Index)
    {
        if(InPos.x() >= XOfFrame(Track->Events[Index].Frame) - 4 && InPos.x() <= XOfFrame(Track->Events[Index].Frame) + 12)
        {
            return { SequencerItem::EKind::Event, R.Track, 0, int(Index) };
        }
    }
    /* Sections: the later one wins where they overlap; its edges trim. */
    for(int Index = int(Track->Sections.size()) - 1; Index >= 0; --Index)
    {
        const auto& Section = Track->Sections[size_t(Index)];
        const double Left = XOfFrame(Section.Start), Right = XOfFrame(Section.End);
        if(InPos.x() < Left - EDGE_HIT || InPos.x() > Right + EDGE_HIT) { continue; }
        if(bOutEdgeStart && std::fabs(InPos.x() - Left) <= EDGE_HIT)  { *bOutEdgeStart = true; }
        else if(bOutEdgeEnd && std::fabs(InPos.x() - Right) <= EDGE_HIT) { *bOutEdgeEnd = true; }
        else if(InPos.x() < Left || InPos.x() > Right) { continue; }
        return { SequencerItem::EKind::Section, R.Track, 0, Index };
    }
    return None;
}

std::vector<SequencerItem> SequencerTimeline::ItemsInRect(const QRect& InRect) const
{
    std::vector<SequencerItem> Found;
    for(int RowIndex = ScrollRow; RowIndex < int(Rows.size()); ++RowIndex)
    {
        const QRect Rect = RowRect(RowIndex);
        if(!Rect.intersects(InRect)) { continue; }
        const Row& R = Rows[size_t(RowIndex)];
        const SequenceTrack* Track = R.Track.IsValid() ? Document.FindTrack(R.Track) : nullptr;
        if(!Track) { continue; }
        const auto Inside = [&](const int InFrame) { const double X = XOfFrame(InFrame); return X >= InRect.left() && X <= InRect.right(); };
        const auto AddKeys = [&](const int InChannel)
        {
            const auto& Keys = Track->Channels[size_t(InChannel)].Keys;
            for(size_t Index = 0; Index < Keys.size(); ++Index)
            {
                const SequencerItem Item{ SequencerItem::EKind::Key, R.Track, InChannel, int(Index) };
                if(Inside(Keys[Index].Frame) && std::find(Found.begin(), Found.end(), Item) == Found.end()) { Found.push_back(Item); }
            }
        };
        if(R.Kind == Row::EKind::Channel)                    { AddKeys(R.Channel); }
        else if(Track->Type == ESequenceTrackType::Transform) { for(size_t C = 0; C < Track->Channels.size(); ++C) { AddKeys(int(C)); } }
        else if(!Track->Channels.empty())                     { AddKeys(0); }
        for(size_t Index = 0; Index < Track->Events.size(); ++Index)
        {
            if(Inside(Track->Events[Index].Frame)) { Found.push_back({ SequencerItem::EKind::Event, R.Track, 0, int(Index) }); }
        }
        for(size_t Index = 0; Index < Track->Sections.size(); ++Index)
        {
            if(Inside(Track->Sections[Index].Start)) { Found.push_back({ SequencerItem::EKind::Section, R.Track, 0, int(Index) }); }
        }
    }
    return Found;
}

int SequencerTimeline::ItemFrame(const SequencerItem& InItem) const
{
    const Sequencer::SequenceAsset& Asset = Document.GetAsset();
    if(InItem.Kind == SequencerItem::EKind::Marker)
    {
        return InItem.Index >= 0 && size_t(InItem.Index) < Asset.Markers.size() ? Asset.Markers[size_t(InItem.Index)].Frame : 0;
    }
    const SequenceTrack* Track = Document.FindTrack(InItem.Track);
    if(!Track || InItem.Index < 0) { return 0; }
    switch(InItem.Kind)
    {
        case SequencerItem::EKind::Key:     return Track->Channels[size_t(InItem.Channel)].Keys[size_t(InItem.Index)].Frame;
        case SequencerItem::EKind::Section: return Track->Sections[size_t(InItem.Index)].Start;
        case SequencerItem::EKind::Event:   return Track->Events[size_t(InItem.Index)].Frame;
        case SequencerItem::EKind::Marker:  break;
    }
    return 0;
}

// === Dragging ====================================================================

void SequencerTimeline::ApplyMove(const int InDelta)
{
    Sequencer::SequenceAsset& Asset = Document.GetLiveAsset();
    for(size_t Index = 0; Index < DragItems.size(); ++Index)
    {
        const SequencerItem& Item = DragItems[Index];
        const int Frame = std::max(0, DragStarts[Index] + InDelta);
        if(Item.Kind == SequencerItem::EKind::Marker)
        {
            Asset.Markers[size_t(Item.Index)].Frame = Frame;
            continue;
        }
        SequenceTrack* Track = Document.FindLiveTrack(Item.Track);
        if(!Track) { continue; }
        switch(Item.Kind)
        {
            case SequencerItem::EKind::Key:   Track->Channels[size_t(Item.Channel)].Keys[size_t(Item.Index)].Frame = Frame; break;
            case SequencerItem::EKind::Event: Track->Events[size_t(Item.Index)].Frame = Frame; break;
            case SequencerItem::EKind::Section:
            {
                auto& Section = Track->Sections[size_t(Item.Index)];
                const int Length = Section.GetLength();
                Section.Start = Frame;
                Section.End   = Frame + Length;
                break;
            }
            case SequencerItem::EKind::Marker: break;
        }
    }
}

void SequencerTimeline::ApplyTrim(const int InDelta)
{
    if(DragItems.empty()) { return; }
    SequenceTrack* Track = Document.FindLiveTrack(DragItems[0].Track);
    if(!Track) { return; }
    auto& Section = Track->Sections[size_t(DragItems[0].Index)];
    const auto& Start = TrimStartSection;
    if(Drag == EDrag::TrimStart)
    {
        /* The start moves; what plays stays where it was in the asset. */
        const int NewStart = std::clamp(Start.Start + InDelta, 0, Start.End - 1);
        const bool bAnchored = Track->Type == ESequenceTrackType::Animation || Track->Type == ESequenceTrackType::Audio
                            || Track->Type == ESequenceTrackType::SubSequence;
        Section.Start  = NewStart;
        Section.Offset = bAnchored ? std::max(0, Start.Offset + (NewStart - Start.Start)) : Start.Offset;
    }
    else
    {
        Section.End = std::max(Start.Start + 1, Start.End + InDelta);
    }
    TrimResultStart = Section.Start;
}

void SequencerTimeline::ReselectMoved(const int InDelta)
{
    /* Each item found again in its track by the frame it went to. */
    std::vector<SequencerItem> Found;
    const Sequencer::SequenceAsset& Asset = Document.GetAsset();
    for(size_t Index = 0; Index < DragItems.size(); ++Index)
    {
        const SequencerItem& Item = DragItems[Index];
        const int Frame = Drag == EDrag::Move ? std::max(0, DragStarts[Index] + InDelta) : TrimResultStart;
        SequencerItem Again = Item;
        Again.Index = -1;
        if(Item.Kind == SequencerItem::EKind::Marker)
        {
            for(size_t M = 0; M < Asset.Markers.size(); ++M) { if(Asset.Markers[M].Frame == Frame) { Again.Index = int(M); } }
        }
        else if(const SequenceTrack* Track = Document.FindTrack(Item.Track))
        {
            if(Item.Kind == SequencerItem::EKind::Key)
            {
                const auto& Keys = Track->Channels[size_t(Item.Channel)].Keys;
                for(size_t K = 0; K < Keys.size(); ++K) { if(Keys[K].Frame == Frame) { Again.Index = int(K); } }
            }
            else if(Item.Kind == SequencerItem::EKind::Event)
            {
                for(size_t E = 0; E < Track->Events.size(); ++E) { if(Track->Events[E].Frame == Frame) { Again.Index = int(E); } }
            }
            else
            {
                for(size_t S = 0; S < Track->Sections.size(); ++S)
                {
                    if(Track->Sections[S].Start == Frame && Again.Index < 0) { Again.Index = int(S); }
                }
            }
        }
        if(Again.Index >= 0 && std::find(Found.begin(), Found.end(), Again) == Found.end()) { Found.push_back(Again); }
    }
    Document.SetSelection(Found);
}

void SequencerTimeline::ScrubTo(const double InX)
{
    const double Frame = FrameAtX(InX);
    Document.SetPlayhead(bSnap ? std::round(Frame) : Frame);
}

// === Mouse =======================================================================

void SequencerTimeline::mousePressEvent(QMouseEvent* InEvent)
{
    setFocus();
    const QPoint Pos = InEvent->position().toPoint();
    PressPos   = Pos;
    DragPos    = Pos;
    bDragMoved = false;
    LastDelta  = 0;
    if(InEvent->button() == Qt::MiddleButton)
    {
        Drag          = EDrag::Pan;
        PanStartFrame = ScrollFrame;
        PanStartRow   = ScrollRow;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if(InEvent->button() != Qt::LeftButton) { return; }

    const bool bCtrl = InEvent->modifiers() & Qt::ControlModifier;

    /* The names: pick the row, or fold it. */
    if(Pos.x() < NAME_WIDTH)
    {
        const int RowIndex = RowAt(Pos.y());
        if(RowIndex < 0) { return; }
        const Row& R = Rows[size_t(RowIndex)];
        const int Indent = 8 + R.Depth * INDENT;
        if(!R.CollapseKey.isEmpty() && Pos.x() >= Indent && Pos.x() < Indent + ARROW_W)
        {
            Document.SetCollapsed(R.CollapseKey, !Document.IsCollapsed(R.CollapseKey));
            return;
        }
        Document.SetCurrent(R.Binding, R.Kind == Row::EKind::Binding ? SequencerTrackRef() : R.Track);
        return;
    }

    bool bEdgeStart = false, bEdgeEnd = false;
    const SequencerItem Hit = HitItem(Pos, &bEdgeStart, &bEdgeEnd);

    if(Hit.Index < 0 && Pos.y() < RULER_HEIGHT)
    {
        Drag = EDrag::Scrub;
        ScrubTo(Pos.x());
        return;
    }
    if(Hit.Index < 0)
    {
        Drag    = EDrag::Box;
        BoxBase = bCtrl ? Document.GetSelection() : std::vector<SequencerItem>();
        if(!bCtrl) { Document.ClearSelection(); }
        const int RowIndex = RowAt(Pos.y());
        if(RowIndex >= 0) { Document.SetCurrent(Rows[size_t(RowIndex)].Binding, Rows[size_t(RowIndex)].Track); }
        return;
    }

    /* A Transform track's summary key stands for every channel key on its frame. */
    std::vector<SequencerItem> Hits = { Hit };
    const int RowIndex = RowAt(Pos.y());
    if(Hit.Kind == SequencerItem::EKind::Key && RowIndex >= 0 && Rows[size_t(RowIndex)].Kind == Row::EKind::Track)
    {
        if(const SequenceTrack* Track = Document.FindTrack(Hit.Track); Track && Track->Type == ESequenceTrackType::Transform)
        {
            Hits.clear();
            const int Frame = ItemFrame(Hit);
            for(size_t Channel = 0; Channel < Track->Channels.size(); ++Channel)
            {
                const auto& Keys = Track->Channels[Channel].Keys;
                for(size_t Index = 0; Index < Keys.size(); ++Index)
                {
                    if(Keys[Index].Frame == Frame) { Hits.push_back({ SequencerItem::EKind::Key, Hit.Track, int(Channel), int(Index) }); }
                }
            }
        }
    }

    std::vector<SequencerItem> Selection = Document.GetSelection();
    const bool bAlreadySelected = Document.IsSelected(Hit);
    if(bCtrl)
    {
        for(const SequencerItem& Each : Hits)
        {
            const auto Found = std::find(Selection.begin(), Selection.end(), Each);
            if(Found != Selection.end()) { Selection.erase(Found); } else { Selection.push_back(Each); }
        }
        Document.SetSelection(Selection);
        return;
    }
    if(!bAlreadySelected)
    {
        Selection = Hits;
        Document.SetSelection(Selection);
    }

    /* Ready to drag: trim an edge, or move what is selected. */
    DragItems = (bEdgeStart || bEdgeEnd) ? std::vector<SequencerItem>{ Hit } : Document.GetSelection();
    if(bEdgeStart || bEdgeEnd)
    {
        Drag = bEdgeStart ? EDrag::TrimStart : EDrag::TrimEnd;
        TrimStartSection = Document.FindTrack(Hit.Track)->Sections[size_t(Hit.Index)];
        TrimResultStart  = TrimStartSection.Start;
    }
    else
    {
        Drag = EDrag::Move;
    }
    DragStarts.clear();
    for(const SequencerItem& Item : DragItems) { DragStarts.push_back(ItemFrame(Item)); }
    Document.BeginDrag();

    /* Alt: the drag moves copies, the originals stay. */
    if(Drag == EDrag::Move && (InEvent->modifiers() & Qt::AltModifier))
    {
        Sequencer::SequenceAsset& Live = Document.GetLiveAsset();
        std::vector<SequencerItem> Copies;
        for(const SequencerItem& Item : DragItems)
        {
            SequencerItem Copy = Item;
            if(Item.Kind == SequencerItem::EKind::Marker)
            {
                Live.Markers.push_back(Live.Markers[size_t(Item.Index)]);
                Copy.Index = int(Live.Markers.size()) - 1;
            }
            else if(SequenceTrack* Track = Document.FindLiveTrack(Item.Track))
            {
                if(Item.Kind == SequencerItem::EKind::Key)
                {
                    auto& Keys = Track->Channels[size_t(Item.Channel)].Keys;
                    Keys.push_back(Keys[size_t(Item.Index)]);
                    Copy.Index = int(Keys.size()) - 1;
                }
                else if(Item.Kind == SequencerItem::EKind::Event)
                {
                    Track->Events.push_back(Track->Events[size_t(Item.Index)]);
                    Copy.Index = int(Track->Events.size()) - 1;
                }
                else
                {
                    Track->Sections.push_back(Track->Sections[size_t(Item.Index)]);
                    Copy.Index = int(Track->Sections.size()) - 1;
                }
            }
            Copies.push_back(Copy);
        }
        DragItems = Copies;
    }
}

void SequencerTimeline::mouseMoveEvent(QMouseEvent* InEvent)
{
    const QPoint Pos = InEvent->position().toPoint();

    /* The cursor says what a press would do. */
    if(Drag == EDrag::None)
    {
        bool bStart = false, bEnd = false;
        HitItem(Pos, &bStart, &bEnd);
        setCursor(bStart || bEnd ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }

    DragPos = Pos;
    if(!bDragMoved && (Pos - PressPos).manhattanLength() < 3) { return; }
    bDragMoved = true;

    const int Delta = int(std::lround(double(Pos.x() - PressPos.x()) / PixelsPerFrame));
    switch(Drag)
    {
        case EDrag::Scrub:
            ScrubTo(Pos.x());
            break;
        case EDrag::Move:
            if(Delta != LastDelta)
            {
                ApplyMove(Delta);
                LastDelta = Delta;
                Document.PreviewDrag();
            }
            break;
        case EDrag::TrimStart:
        case EDrag::TrimEnd:
            if(Delta != LastDelta)
            {
                ApplyTrim(Delta);
                LastDelta = Delta;
                Document.PreviewDrag();
            }
            break;
        case EDrag::Pan:
        {
            SetScrollFrame(PanStartFrame - double(Pos.x() - PressPos.x()) / PixelsPerFrame);
            ScrollRow = std::clamp(PanStartRow - (Pos.y() - PressPos.y()) / TRACK_HEIGHT, 0, std::max(0, int(Rows.size()) - 1));
            update();
            break;
        }
        case EDrag::Box:
        {
            std::vector<SequencerItem> Items = BoxBase;
            for(const SequencerItem& Each : ItemsInRect(QRect(PressPos, Pos).normalized()))
            {
                if(std::find(Items.begin(), Items.end(), Each) == Items.end()) { Items.push_back(Each); }
            }
            Document.SetSelection(Items);
            update();
            break;
        }
        case EDrag::None:
            break;
    }
}

void SequencerTimeline::mouseReleaseEvent(QMouseEvent* InEvent)
{
    (void)InEvent;
    const EDrag Was = Drag;
    if(Was == EDrag::Move || Was == EDrag::TrimStart || Was == EDrag::TrimEnd)
    {
        const bool bCopy = Was == EDrag::Move && Document.GetSelection() != DragItems;
        const QString Text = Was != EDrag::Move ? QStringLiteral("Trim Section")
                           : bCopy             ? QStringLiteral("Copy")
                                               : QStringLiteral("Move");
        if(!bDragMoved && !bCopy)
        {
            Document.EndDrag(Text, true);
            /* A click on a selected item without moving: it alone. */
            const SequencerItem Hit = HitItem(PressPos);
            if(Hit.Index >= 0 && Was == EDrag::Move) { Document.SetSelection({ Hit }); }
        }
        else
        {
            Document.EndDrag(Text);
            Drag = Was;
            ReselectMoved(LastDelta);
        }
    }
    if(Was == EDrag::Pan) { setCursor(Qt::ArrowCursor); }
    Drag = EDrag::None;
    DragItems.clear();
    update();
}

void SequencerTimeline::mouseDoubleClickEvent(QMouseEvent* InEvent)
{
    const QPoint Pos = InEvent->position().toPoint();
    const int RowIndex = RowAt(Pos.y());
    if(Pos.x() < NAME_WIDTH)
    {
        if(RowIndex >= 0) { emit RowActivated(RowIndex); }
        return;
    }
    if(RowIndex < 0) { return; }
    const Row& R = Rows[size_t(RowIndex)];
    const SequenceTrack* Track = R.Track.IsValid() ? Document.FindTrack(R.Track) : nullptr;
    if(!Track || HitItem(Pos).Index >= 0) { return; }

    const int Frame = std::max(0, int(std::lround(FrameAtX(Pos.x()))));
    const uint32_t Fps = std::max<uint32_t>(Document.GetAsset().FrameRate, 1);
    switch(Track->Type)
    {
        case ESequenceTrackType::Transform:
            if(R.Kind == Row::EKind::Channel)
            {
                Document.SetKey(R.Track, R.Channel, Frame, Document.EvaluateChannel(R.Track, R.Channel, Frame, 0.0f), QStringLiteral("Add Key"));
            }
            break;
        case ESequenceTrackType::Float:
        case ESequenceTrackType::Bool:
        case ESequenceTrackType::Fade:
        case ESequenceTrackType::PostProcessSetting:
            Document.SetKey(R.Track, 0, Frame, Document.EvaluateChannel(R.Track, 0, Frame, Track->Type == ESequenceTrackType::Bool ? 1.0f : 0.0f),
                            QStringLiteral("Add Key"));
            break;
        case ESequenceTrackType::BusVolume:
            Document.SetKey(R.Track, 0, Frame, Document.EvaluateChannel(R.Track, 0, Frame, 1.0f), QStringLiteral("Add Key"));
            break;
        case ESequenceTrackType::Subtitle:
            Document.AddSection(R.Track, Frame, Frame + int(Fps) * 3, QStringLiteral("..."));
            break;
        case ESequenceTrackType::Event:
            Document.AddEvent(R.Track, Frame, QStringLiteral("Event"));
            break;
        case ESequenceTrackType::CameraCut:
        {
            /* The first camera binding, for a start. */
            const auto& Bindings = Document.GetAsset().Bindings;
            Document.AddSection(R.Track, Frame, Frame + int(Fps) * 2, Bindings.empty() ? QString() : QString::fromStdString(Bindings.front().Id));
            break;
        }
        case ESequenceTrackType::Animation:
        case ESequenceTrackType::Audio:
        case ESequenceTrackType::SubSequence:
        case ESequenceTrackType::PostProcess:
            Document.AddSection(R.Track, Frame, Frame + int(Fps) * 2, QString());
            break;
        case ESequenceTrackType::Count:
            break;
    }
}

void SequencerTimeline::wheelEvent(QWheelEvent* InEvent)
{
    const double Steps = InEvent->angleDelta().y() / 120.0;
    const bool bOverRuler = InEvent->position().y() < RULER_HEIGHT && InEvent->position().x() >= NAME_WIDTH;
    if((InEvent->modifiers() & Qt::ControlModifier) || bOverRuler)
    {
        /* Zoom about the frame under the cursor. */
        ZoomBy(std::pow(1.2, Steps), std::max(double(NAME_WIDTH), InEvent->position().x()));
    }
    else if(InEvent->modifiers() & Qt::ShiftModifier)
    {
        SetScrollFrame(ScrollFrame - Steps * 60.0 / PixelsPerFrame);
    }
    else
    {
        ScrollRow = std::clamp(ScrollRow - int(std::lround(Steps * 2)), 0, std::max(0, int(Rows.size()) - 1));
    }
    update();
    InEvent->accept();
}

void SequencerTimeline::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->key() == Qt::Key_Delete || InEvent->key() == Qt::Key_Backspace)
    {
        Document.DeleteSelection();
        return;
    }
    if(InEvent->key() == Qt::Key_Equal || InEvent->key() == Qt::Key_Plus)
    {
        ZoomBy(1.5);
        return;
    }
    if(InEvent->key() == Qt::Key_Minus || InEvent->key() == Qt::Key_Underscore)
    {
        ZoomBy(1.0 / 1.5);
        return;
    }
    if(InEvent->key() == Qt::Key_F)
    {
        FitAll();
        return;
    }
    if(InEvent->key() == Qt::Key_Escape && Drag != EDrag::None)
    {
        if(Drag == EDrag::Move || Drag == EDrag::TrimStart || Drag == EDrag::TrimEnd) { Document.EndDrag(QString(), true); }
        Drag = EDrag::None;
        update();
        return;
    }
    QWidget::keyPressEvent(InEvent);
}

void SequencerTimeline::contextMenuEvent(QContextMenuEvent* InEvent)
{
    const int RowIndex = InEvent->pos().y() < RULER_HEIGHT ? -1 : RowAt(InEvent->pos().y());
    if(RowIndex >= 0)
    {
        const Row& R = Rows[size_t(RowIndex)];
        Document.SetCurrent(R.Binding, R.Kind == Row::EKind::Binding ? SequencerTrackRef() : R.Track);
    }
    emit ContextMenuRequested(RowIndex, InEvent->pos().x() >= NAME_WIDTH ? std::max(0.0, FrameAtX(InEvent->pos().x())) : Document.GetPlayhead(),
                              InEvent->globalPos());
}

void SequencerTimeline::resizeEvent(QResizeEvent* InEvent)
{
    QWidget::resizeEvent(InEvent);
    if(bFitPending && isVisible() && width() > NAME_WIDTH + 100)
    {
        FitAll();
        return;
    }
    emit ViewChanged();   // how many frames the lanes show
}

#endif // WITH_EDITOR
