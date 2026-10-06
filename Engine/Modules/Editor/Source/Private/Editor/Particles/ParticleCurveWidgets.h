#ifndef NIMBLEEDITOR_PARTICLECURVEWIDGETS_H
#define NIMBLEEDITOR_PARTICLECURVEWIDGETS_H

#if WITH_EDITOR

#include <vector>

#include <QWidget>

#include "Public/RenderCore/ParticleSystemAsset.h"

class QDoubleSpinBox;
class QPushButton;
class QToolButton;

/*
 * The Particle Editor's over-life editors: a colour gradient and a size
 * curve, each up to RenderCore::PARTICLE_CURVE_KEYS_MAX keys over a life
 * (0 birth, 1 death), as ParticleEmitterDesc holds them.
 *
 *   click a key          select it; drag it (a gradient's along, a curve's anywhere)
 *   double-click         add a key there
 *   right-click a key    remove it (never the last)
 *   below                the selected key's time and value (colour), Add, Remove
 *
 * Edited(false) while a drag is under way, Edited(true) once an edit is
 * done: the panel previews the first and records the second as one undo
 * step. SetKeys is ignored mid-drag.
 */
class ParticleKeyEditBase : public QWidget
{
    Q_OBJECT

public:
    explicit ParticleKeyEditBase(QWidget* InParent);

    bool IsDragging() const { return Dragging >= 0; }

signals:
    void Edited(bool bInFinished);

protected:
    /* The strip keys are drawn in, and a key's time as an x in it. */
    QRect StripRect() const;
    int   TimeToX(float InTime) const;
    float XToTime(int InX) const;

    virtual int   KeyCount() const = 0;
    virtual float KeyTime(int InIndex) const = 0;
    virtual void  SetKeyTime(int InIndex, float InTime) = 0;
    /* The key whose marker is under InAt, or -1. */
    virtual int   KeyAt(const QPoint& InAt) const = 0;
    /* Drag key InIndex to InAt. */
    virtual void  DragKey(int InIndex, const QPoint& InAt) = 0;
    /* A key at InTime, the value the keys already give there; selected. */
    virtual void  AddKeyAtTime(float InTime) = 0;
    /* A key where the strip was double-clicked: AddKeyAtTime of its x. */
    virtual void  AddKeyAt(const QPoint& InAt) { AddKeyAtTime(XToTime(InAt.x())); }
    /* Double-clicked: a gradient's key opens its colour. */
    virtual void  ActivateKey(int InIndex) { (void)InIndex; }
    virtual void  RemoveKey(int InIndex) = 0;
    /* Keys back in time order, keeping Selected on the same key. */
    virtual void  SortKeys() = 0;
    virtual void  SyncFields() = 0;

    void Select(int InIndex);
    void Finish();

    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;

    /* The row under the strip: time, value, Add, Remove. */
    QDoubleSpinBox* TimeBox     = nullptr;
    QToolButton*    AddButton   = nullptr;
    QToolButton*    RemoveButton = nullptr;
    int             StripHeight = 32;
    int             Selected    = 0;
    int             Dragging    = -1;
    bool            bSyncing    = false;
};

/* A colour gradient: alpha shown over a checkerboard. */
class ParticleGradientEdit final : public ParticleKeyEditBase
{
    Q_OBJECT

public:
    explicit ParticleGradientEdit(QWidget* InParent = nullptr);

    void SetKeys(const RenderCore::ParticleColorKey* InKeys, int InCount);
    const std::vector<RenderCore::ParticleColorKey>& GetKeys() const { return Keys; }

    QSize sizeHint() const override { return QSize(240, StripHeight + 46); }

protected:
    void paintEvent(QPaintEvent* InEvent) override;

    int   KeyCount() const override { return int(Keys.size()); }
    float KeyTime(int InIndex) const override { return Keys[size_t(InIndex)].Time; }
    void  SetKeyTime(int InIndex, float InTime) override { Keys[size_t(InIndex)].Time = InTime; }
    int   KeyAt(const QPoint& InAt) const override;
    void  DragKey(int InIndex, const QPoint& InAt) override;
    void  AddKeyAtTime(float InTime) override;
    void  ActivateKey(int InIndex) override { Select(InIndex); PickColor(); }
    void  RemoveKey(int InIndex) override;
    void  SortKeys() override;
    void  SyncFields() override;

private:
    /* The gradient's colour at InTime: straight between keys, held past the ends. */
    EngineCore::Color ColorAt(float InTime) const;
    void              PickColor();

    std::vector<RenderCore::ParticleColorKey> Keys;
    QPushButton* ColorButton = nullptr;
};

/* A size curve: straight between keys, plotted against a scale that fits them. */
class ParticleCurveEdit final : public ParticleKeyEditBase
{
    Q_OBJECT

public:
    explicit ParticleCurveEdit(QWidget* InParent = nullptr);

    void SetKeys(const RenderCore::ParticleSizeKey* InKeys, int InCount);
    const std::vector<RenderCore::ParticleSizeKey>& GetKeys() const { return Keys; }

    QSize sizeHint() const override { return QSize(240, StripHeight + 46); }

protected:
    void paintEvent(QPaintEvent* InEvent) override;

    int   KeyCount() const override { return int(Keys.size()); }
    float KeyTime(int InIndex) const override { return Keys[size_t(InIndex)].Time; }
    void  SetKeyTime(int InIndex, float InTime) override { Keys[size_t(InIndex)].Time = InTime; }
    int   KeyAt(const QPoint& InAt) const override;
    void  DragKey(int InIndex, const QPoint& InAt) override;
    void  AddKeyAtTime(float InTime) override;
    void  AddKeyAt(const QPoint& InAt) override;
    void  RemoveKey(int InIndex) override;
    void  SortKeys() override;
    void  SyncFields() override;

private:
    float ValueAt(float InTime) const;
    int   ValueToY(float InValue) const;
    float YToValue(int InY) const;
    /* The plot's top: a little over the largest key, kept while dragging so the plot holds still. */
    void  FitScale();

    std::vector<RenderCore::ParticleSizeKey> Keys;
    QDoubleSpinBox* ValueBox = nullptr;
    float           Top      = 20.0f;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLECURVEWIDGETS_H
