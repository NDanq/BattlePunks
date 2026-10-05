#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleCurveWidgets.h"

#include <algorithm>
#include <cmath>

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

using RenderCore::PARTICLE_CURVE_KEYS_MAX;
using RenderCore::ParticleColorKey;
using RenderCore::ParticleSizeKey;

namespace
{
    constexpr int MARGIN      = 8;
    constexpr int PICK_RADIUS = 7;

    QColor ToQColor(const EngineCore::Color InColor) { return QColor(InColor.R, InColor.G, InColor.B, InColor.A); }

    EngineCore::Color FromQColor(const QColor& InColor)
    {
        return EngineCore::Color(uint8_t(InColor.red()), uint8_t(InColor.green()), uint8_t(InColor.blue()), uint8_t(InColor.alpha()));
    }

    /* A key's time, to the hundredth the time box shows. */
    float Round(const float InTime) { return std::clamp(std::round(InTime * 1000.0f) / 1000.0f, 0.0f, 1.0f); }
}

// === The shared strip ==============================================================

ParticleKeyEditBase::ParticleKeyEditBase(QWidget* InParent)
    : QWidget(InParent)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);

    TimeBox = new QDoubleSpinBox(this);
    TimeBox->setRange(0.0, 1.0);
    TimeBox->setSingleStep(0.05);
    TimeBox->setDecimals(3);
    TimeBox->setToolTip(QStringLiteral("The selected key's time: 0 at birth, 1 at death"));
    AddButton = new QToolButton(this);
    AddButton->setText(QStringLiteral("+"));
    AddButton->setToolTip(QStringLiteral("Add a key after the selected one (or double-click the strip)"));
    RemoveButton = new QToolButton(this);
    RemoveButton->setText(QString(QChar(0x2212)));
    RemoveButton->setToolTip(QStringLiteral("Remove the selected key (or right-click it)"));

    connect(TimeBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        if(bSyncing || Selected < 0 || Selected >= KeyCount()) { return; }
        SetKeyTime(Selected, float(InValue));
        SortKeys();
        Finish();
    });
    connect(AddButton, &QToolButton::clicked, this, [this]()
    {
        if(KeyCount() >= int(PARTICLE_CURVE_KEYS_MAX)) { return; }
        const float From = Selected >= 0 && Selected < KeyCount() ? KeyTime(Selected) : 0.0f;
        const float To   = Selected + 1 < KeyCount() ? KeyTime(Selected + 1) : std::min(From + 0.2f, 1.0f);
        AddKeyAtTime(Round(0.5f * (From + To)));
        Finish();
    });
    connect(RemoveButton, &QToolButton::clicked, this, [this]()
    {
        if(KeyCount() > 1 && Selected >= 0 && Selected < KeyCount())
        {
            RemoveKey(Selected);
            Finish();
        }
    });
}

QRect ParticleKeyEditBase::StripRect() const
{
    return QRect(MARGIN, 4, std::max(width() - 2 * MARGIN, 10), StripHeight);
}

int ParticleKeyEditBase::TimeToX(const float InTime) const
{
    const QRect Strip = StripRect();
    return Strip.left() + int(std::lround(std::clamp(InTime, 0.0f, 1.0f) * float(Strip.width())));
}

float ParticleKeyEditBase::XToTime(const int InX) const
{
    const QRect Strip = StripRect();
    return Round(float(InX - Strip.left()) / float(std::max(Strip.width(), 1)));
}

void ParticleKeyEditBase::Select(const int InIndex)
{
    Selected = std::clamp(InIndex, 0, std::max(KeyCount() - 1, 0));
    SyncFields();
    update();
}

void ParticleKeyEditBase::Finish()
{
    SyncFields();
    update();
    emit Edited(true);
}

void ParticleKeyEditBase::mousePressEvent(QMouseEvent* InEvent)
{
    const int Key = KeyAt(InEvent->position().toPoint());
    if(Key < 0) { return; }
    Select(Key);
    if(InEvent->button() == Qt::LeftButton)
    {
        Dragging = Key;
    }
    else if(InEvent->button() == Qt::RightButton && KeyCount() > 1)
    {
        RemoveKey(Key);
        Finish();
    }
}

void ParticleKeyEditBase::mouseMoveEvent(QMouseEvent* InEvent)
{
    if(Dragging < 0) { return; }
    DragKey(Dragging, InEvent->position().toPoint());
    SyncFields();
    update();
    emit Edited(false);
}

void ParticleKeyEditBase::mouseReleaseEvent(QMouseEvent* InEvent)
{
    (void)InEvent;
    if(Dragging < 0) { return; }
    Dragging = -1;
    SortKeys();
    Finish();
}

void ParticleKeyEditBase::mouseDoubleClickEvent(QMouseEvent* InEvent)
{
    const QPoint At = InEvent->position().toPoint();
    const int Key = KeyAt(At);
    if(Key >= 0)
    {
        ActivateKey(Key);
        return;
    }
    if(StripRect().adjusted(-MARGIN, 0, MARGIN, 14).contains(At) && KeyCount() < int(PARTICLE_CURVE_KEYS_MAX))
    {
        AddKeyAt(At);
        Finish();
    }
}

void ParticleKeyEditBase::keyPressEvent(QKeyEvent* InEvent)
{
    if((InEvent->key() == Qt::Key_Delete || InEvent->key() == Qt::Key_Backspace) && KeyCount() > 1)
    {
        RemoveKey(Selected);
        Finish();
        return;
    }
    QWidget::keyPressEvent(InEvent);
}

// === The gradient ==================================================================

ParticleGradientEdit::ParticleGradientEdit(QWidget* InParent)
    : ParticleKeyEditBase(InParent)
{
    StripHeight = 24;
    ColorButton = new QPushButton(this);
    ColorButton->setFixedWidth(52);
    ColorButton->setToolTip(QStringLiteral("The selected key's colour and alpha (or double-click its marker)"));
    connect(ColorButton, &QPushButton::clicked, this, &ParticleGradientEdit::PickColor);

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, StripHeight + 18, 0, 0);
    auto* Row = new QHBoxLayout();
    Row->setContentsMargins(MARGIN, 0, MARGIN, 0);
    Row->addWidget(new QLabel(QStringLiteral("Time"), this));
    Row->addWidget(TimeBox);
    Row->addWidget(ColorButton);
    Row->addStretch(1);
    Row->addWidget(AddButton);
    Row->addWidget(RemoveButton);
    Layout->addLayout(Row);
    setToolTip(QStringLiteral("Colour and alpha over life. Drag a key along; double-click to add one, or a key to pick its colour; "
                              "right-click a key to remove it"));
    SetKeys(nullptr, 0);
}

void ParticleGradientEdit::SetKeys(const ParticleColorKey* InKeys, const int InCount)
{
    if(IsDragging()) { return; }
    Keys.assign(InKeys, InKeys + std::clamp(InCount, 0, int(PARTICLE_CURVE_KEYS_MAX)));
    if(Keys.empty()) { Keys.push_back(ParticleColorKey()); }
    Selected = std::clamp(Selected, 0, int(Keys.size()) - 1);
    SyncFields();
    update();
}

EngineCore::Color ParticleGradientEdit::ColorAt(const float InTime) const
{
    size_t After = 0;
    while(After < Keys.size() && Keys[After].Time < InTime) { ++After; }
    const ParticleColorKey& A = Keys[After == 0 ? 0 : After - 1];
    const ParticleColorKey& B = Keys[std::min(After, Keys.size() - 1)];
    const float Span = B.Time - A.Time;
    const float T    = Span > 1e-6f ? std::clamp((InTime - A.Time) / Span, 0.0f, 1.0f) : 0.0f;
    const auto Mix = [T](const uint8_t InA, const uint8_t InB) { return uint8_t(std::lround(float(InA) + (float(InB) - float(InA)) * T)); };
    return EngineCore::Color(Mix(A.Color.R, B.Color.R), Mix(A.Color.G, B.Color.G), Mix(A.Color.B, B.Color.B), Mix(A.Color.A, B.Color.A));
}

void ParticleGradientEdit::paintEvent(QPaintEvent* InEvent)
{
    (void)InEvent;
    QPainter Painter(this);
    Painter.setRenderHint(QPainter::Antialiasing);
    const QRect Strip = StripRect();

    /* A checkerboard under it, so alpha shows. */
    for(int Y = Strip.top(), Row = 0; Y < Strip.bottom(); Y += 6, ++Row)
    {
        for(int X = Strip.left(), Column = 0; X < Strip.right(); X += 6, ++Column)
        {
            Painter.fillRect(QRect(X, Y, std::min(6, Strip.right() - X), std::min(6, Strip.bottom() - Y)),
                             (Row + Column) % 2 ? QColor(200, 200, 200) : QColor(120, 120, 120));
        }
    }
    QLinearGradient Fill(Strip.left(), 0, Strip.right(), 0);
    for(const ParticleColorKey& Key : Keys) { Fill.setColorAt(Key.Time, ToQColor(Key.Color)); }
    if(Keys.size() == 1) { Fill.setColorAt(0.0, ToQColor(Keys[0].Color)); Fill.setColorAt(1.0, ToQColor(Keys[0].Color)); }
    Painter.fillRect(Strip, Fill);
    Painter.setPen(palette().color(QPalette::Mid));
    Painter.drawRect(Strip);

    /* A marker under each key, filled with its colour; the selected one outlined. */
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        const int X = TimeToX(Keys[size_t(Index)].Time);
        QPainterPath Marker;
        Marker.moveTo(X, Strip.bottom() + 2);
        Marker.lineTo(X - 6, Strip.bottom() + 12);
        Marker.lineTo(X + 6, Strip.bottom() + 12);
        Marker.closeSubpath();
        QColor Solid = ToQColor(Keys[size_t(Index)].Color);
        Solid.setAlpha(255);
        Painter.fillPath(Marker, Solid);
        Painter.setPen(QPen(Index == Selected ? palette().color(QPalette::Highlight) : palette().color(QPalette::WindowText),
                            Index == Selected ? 2.0 : 1.0));
        Painter.drawPath(Marker);
    }
}

int ParticleGradientEdit::KeyAt(const QPoint& InAt) const
{
    const QRect Strip = StripRect();
    if(InAt.y() < Strip.top() || InAt.y() > Strip.bottom() + 14) { return -1; }
    int Best = -1, BestDistance = PICK_RADIUS + 1;
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        const int Distance = std::abs(TimeToX(Keys[size_t(Index)].Time) - InAt.x());
        if(Distance < BestDistance) { Best = Index; BestDistance = Distance; }
    }
    return Best;
}

void ParticleGradientEdit::DragKey(const int InIndex, const QPoint& InAt)
{
    Keys[size_t(InIndex)].Time = XToTime(InAt.x());
}

void ParticleGradientEdit::AddKeyAtTime(const float InTime)
{
    const ParticleColorKey Key{ InTime, ColorAt(InTime) };
    Keys.push_back(Key);
    SortKeys();
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        if(Keys[size_t(Index)].Time == InTime) { Selected = Index; }
    }
}

void ParticleGradientEdit::RemoveKey(const int InIndex)
{
    if(Keys.size() <= 1 || InIndex < 0 || InIndex >= int(Keys.size())) { return; }
    Keys.erase(Keys.begin() + InIndex);
    Selected = std::min(InIndex, int(Keys.size()) - 1);
}

void ParticleGradientEdit::SortKeys()
{
    const ParticleColorKey Chosen = Keys[size_t(std::clamp(Selected, 0, int(Keys.size()) - 1))];
    std::stable_sort(Keys.begin(), Keys.end(), [](const ParticleColorKey& A, const ParticleColorKey& B) { return A.Time < B.Time; });
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        if(Keys[size_t(Index)].Time == Chosen.Time && Keys[size_t(Index)].Color.R == Chosen.Color.R
           && Keys[size_t(Index)].Color.A == Chosen.Color.A) { Selected = Index; break; }
    }
}

void ParticleGradientEdit::SyncFields()
{
    const QSignalBlocker Block(TimeBox);
    bSyncing = true;
    const ParticleColorKey& Key = Keys[size_t(std::clamp(Selected, 0, int(Keys.size()) - 1))];
    TimeBox->setValue(Key.Time);
    ColorButton->setStyleSheet(QStringLiteral("background-color: rgba(%1, %2, %3, %4); border: 1px solid palette(mid);")
                                   .arg(Key.Color.R).arg(Key.Color.G).arg(Key.Color.B).arg(Key.Color.A));
    ColorButton->setText(QString::number(Key.Color.A));
    AddButton->setEnabled(Keys.size() < PARTICLE_CURVE_KEYS_MAX);
    RemoveButton->setEnabled(Keys.size() > 1);
    bSyncing = false;
}

void ParticleGradientEdit::PickColor()
{
    if(Keys.empty()) { return; }
    ParticleColorKey& Key = Keys[size_t(std::clamp(Selected, 0, int(Keys.size()) - 1))];
    const QColor Chosen = QColorDialog::getColor(ToQColor(Key.Color), this, QStringLiteral("Key Colour"), QColorDialog::ShowAlphaChannel);
    if(!Chosen.isValid()) { return; }
    Key.Color = FromQColor(Chosen);
    Finish();
}

// === The curve =====================================================================

ParticleCurveEdit::ParticleCurveEdit(QWidget* InParent)
    : ParticleKeyEditBase(InParent)
{
    StripHeight = 96;
    ValueBox = new QDoubleSpinBox(this);
    ValueBox->setRange(0.0, 1.0e6);
    ValueBox->setDecimals(2);
    ValueBox->setToolTip(QStringLiteral("The selected key's size, world units across"));
    connect(ValueBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        if(bSyncing || Selected < 0 || Selected >= int(Keys.size())) { return; }
        Keys[size_t(Selected)].Value = float(InValue);
        FitScale();
        Finish();
    });

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, StripHeight + 10, 0, 0);
    auto* Row = new QHBoxLayout();
    Row->setContentsMargins(MARGIN, 0, MARGIN, 0);
    Row->addWidget(new QLabel(QStringLiteral("Time"), this));
    Row->addWidget(TimeBox);
    Row->addWidget(new QLabel(QStringLiteral("Size"), this));
    Row->addWidget(ValueBox);
    Row->addStretch(1);
    Row->addWidget(AddButton);
    Row->addWidget(RemoveButton);
    Layout->addLayout(Row);
    setToolTip(QStringLiteral("Size over life. Drag a key anywhere; double-click to add one; right-click a key to remove it"));
    SetKeys(nullptr, 0);
}

void ParticleCurveEdit::SetKeys(const ParticleSizeKey* InKeys, const int InCount)
{
    if(IsDragging()) { return; }
    Keys.assign(InKeys, InKeys + std::clamp(InCount, 0, int(PARTICLE_CURVE_KEYS_MAX)));
    if(Keys.empty()) { Keys.push_back(ParticleSizeKey()); }
    Selected = std::clamp(Selected, 0, int(Keys.size()) - 1);
    FitScale();
    SyncFields();
    update();
}

void ParticleCurveEdit::FitScale()
{
    if(IsDragging()) { return; }
    float Largest = 0.0f;
    for(const ParticleSizeKey& Key : Keys) { Largest = std::max(Largest, Key.Value); }
    Top = std::max(Largest * 1.25f, 1.0f);
}

int ParticleCurveEdit::ValueToY(const float InValue) const
{
    const QRect Strip = StripRect();
    return Strip.bottom() - int(std::lround(std::clamp(InValue / Top, 0.0f, 1.0f) * float(Strip.height())));
}

float ParticleCurveEdit::YToValue(const int InY) const
{
    const QRect Strip = StripRect();
    const float Value = float(Strip.bottom() - InY) / float(std::max(Strip.height(), 1)) * Top;
    return std::max(std::round(Value * 100.0f) / 100.0f, 0.0f);
}

float ParticleCurveEdit::ValueAt(const float InTime) const
{
    size_t After = 0;
    while(After < Keys.size() && Keys[After].Time < InTime) { ++After; }
    const ParticleSizeKey& A = Keys[After == 0 ? 0 : After - 1];
    const ParticleSizeKey& B = Keys[std::min(After, Keys.size() - 1)];
    const float Span = B.Time - A.Time;
    const float T    = Span > 1e-6f ? std::clamp((InTime - A.Time) / Span, 0.0f, 1.0f) : 0.0f;
    return A.Value + (B.Value - A.Value) * T;
}

void ParticleCurveEdit::paintEvent(QPaintEvent* InEvent)
{
    (void)InEvent;
    QPainter Painter(this);
    Painter.setRenderHint(QPainter::Antialiasing);
    const QRect Strip = StripRect();
    Painter.fillRect(Strip, palette().color(QPalette::Base));

    /* Quarters of a life across, quarters of the scale up; the scale's top written in the corner. */
    Painter.setPen(QPen(palette().color(QPalette::Midlight), 1.0, Qt::DotLine));
    for(int Quarter = 1; Quarter < 4; ++Quarter)
    {
        const int X = Strip.left() + Strip.width() * Quarter / 4;
        const int Y = Strip.top() + Strip.height() * Quarter / 4;
        Painter.drawLine(X, Strip.top(), X, Strip.bottom());
        Painter.drawLine(Strip.left(), Y, Strip.right(), Y);
    }
    Painter.setPen(palette().color(QPalette::Mid));
    Painter.drawRect(Strip);
    Painter.setPen(palette().color(QPalette::PlaceholderText));
    Painter.drawText(Strip.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, QString::number(double(Top), 'g', 4));
    Painter.drawText(Strip.adjusted(4, 2, -4, -2), Qt::AlignBottom | Qt::AlignLeft, QStringLiteral("0"));

    /* The curve, held flat before its first key and after its last. */
    QPainterPath Line;
    Line.moveTo(Strip.left(), ValueToY(Keys.front().Value));
    for(const ParticleSizeKey& Key : Keys) { Line.lineTo(TimeToX(Key.Time), ValueToY(Key.Value)); }
    Line.lineTo(Strip.right(), ValueToY(Keys.back().Value));
    Painter.setPen(QPen(palette().color(QPalette::Highlight), 2.0));
    Painter.drawPath(Line);

    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        const QPoint At(TimeToX(Keys[size_t(Index)].Time), ValueToY(Keys[size_t(Index)].Value));
        Painter.setBrush(Index == Selected ? palette().color(QPalette::Highlight) : palette().color(QPalette::Base));
        Painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
        Painter.drawEllipse(At, 5, 5);
    }
}

int ParticleCurveEdit::KeyAt(const QPoint& InAt) const
{
    int Best = -1, BestDistance = PICK_RADIUS * PICK_RADIUS + 1;
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        const int DX = TimeToX(Keys[size_t(Index)].Time) - InAt.x();
        const int DY = ValueToY(Keys[size_t(Index)].Value) - InAt.y();
        if(DX * DX + DY * DY < BestDistance) { Best = Index; BestDistance = DX * DX + DY * DY; }
    }
    return Best;
}

void ParticleCurveEdit::DragKey(const int InIndex, const QPoint& InAt)
{
    Keys[size_t(InIndex)].Time  = XToTime(InAt.x());
    Keys[size_t(InIndex)].Value = YToValue(InAt.y());
}

void ParticleCurveEdit::AddKeyAtTime(const float InTime)
{
    Keys.push_back(ParticleSizeKey{ InTime, ValueAt(InTime) });
    SortKeys();
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        if(Keys[size_t(Index)].Time == InTime) { Selected = Index; }
    }
}

void ParticleCurveEdit::AddKeyAt(const QPoint& InAt)
{
    AddKeyAtTime(XToTime(InAt.x()));
    Keys[size_t(Selected)].Value = YToValue(InAt.y());
}

void ParticleCurveEdit::RemoveKey(const int InIndex)
{
    if(Keys.size() <= 1 || InIndex < 0 || InIndex >= int(Keys.size())) { return; }
    Keys.erase(Keys.begin() + InIndex);
    Selected = std::min(InIndex, int(Keys.size()) - 1);
}

void ParticleCurveEdit::SortKeys()
{
    const ParticleSizeKey Chosen = Keys[size_t(std::clamp(Selected, 0, int(Keys.size()) - 1))];
    std::stable_sort(Keys.begin(), Keys.end(), [](const ParticleSizeKey& A, const ParticleSizeKey& B) { return A.Time < B.Time; });
    for(int Index = 0; Index < int(Keys.size()); ++Index)
    {
        if(Keys[size_t(Index)].Time == Chosen.Time && Keys[size_t(Index)].Value == Chosen.Value) { Selected = Index; break; }
    }
    FitScale();
}

void ParticleCurveEdit::SyncFields()
{
    const QSignalBlocker BlockTime(TimeBox);
    const QSignalBlocker BlockValue(ValueBox);
    bSyncing = true;
    const ParticleSizeKey& Key = Keys[size_t(std::clamp(Selected, 0, int(Keys.size()) - 1))];
    TimeBox->setValue(Key.Time);
    ValueBox->setValue(Key.Value);
    AddButton->setEnabled(Keys.size() < PARTICLE_CURVE_KEYS_MAX);
    RemoveButton->setEnabled(Keys.size() > 1);
    bSyncing = false;
}

#endif // WITH_EDITOR
