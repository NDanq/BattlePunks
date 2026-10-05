#ifndef NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
#define NIMBLEEDITOR_PARTICLEEMITTERPANELS_H

#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QWidget>

class ParticleCurveEdit;
class ParticleDocument;
class ParticleGradientEdit;
class QFormLayout;
class PropertyEditor;
class QLabel;
class QListWidget;
class QToolButton;
class QVBoxLayout;

/*
 * The Particle Editor's Emitters dock: the system's emitters in order, a
 * check box each (Enabled), renamed in place (F2 or double-click); Add,
 * Duplicate, Remove, Up and Down below, and in the context menu. Each
 * emitter is drawn in list order.
 */
class ParticleEmitterListPanel : public QWidget
{
    Q_OBJECT

public:
    ParticleEmitterListPanel(ParticleDocument& InDocument, QWidget* InParent = nullptr);

private:
    void Rebuild();
    void UpdateButtons();

    ParticleDocument& Document;
    QListWidget*      List      = nullptr;
    QToolButton*      AddButton = nullptr;
    QToolButton*      DupButton = nullptr;
    QToolButton*      DelButton = nullptr;
    QToolButton*      UpButton  = nullptr;
    QToolButton*      DownButton = nullptr;
    bool              bRebuilding = false;
};

/*
 * The Particle Editor's Details dock: the selected emitter's settings, in
 * the Details panel's own property widgets (PropertyWidgets.h) grouped as
 * Rendering, Emission, Shape, Motion, Beam and Over Life, each with its
 * reset arrow -- only those its type, modes and shape use -- and a colour
 * gradient and size curve editor (ParticleCurveWidgets.h) when chosen.
 * A footer says how many particles the emitter holds once it has run a while,
 * and warns when Max Particles cuts lives short.
 */
class ParticleEmitterDetailsPanel : public QWidget
{
    Q_OBJECT

public:
    ParticleEmitterDetailsPanel(ParticleDocument& InDocument, QWidget* InParent = nullptr);

private:
    struct Row
    {
        PropertyEditor* Editor = nullptr;
        QToolButton*    Reset  = nullptr;
        size_t          Entry  = 0;   // its setting, in the panel's table
    };

    void Rebuild();
    void Refresh();

    /* A gradient's (or curve's) row: its label, then the editor across the form. */
    void AddKeyEditor(bool bInGradient, const QString& InName, const QString& InTip, QFormLayout* InForm);

    ParticleDocument& Document;
    QWidget*          Content = nullptr;
    QVBoxLayout*      ContentLayout = nullptr;
    QLabel*           Budget  = nullptr;
    std::vector<Row>  Rows;
    uint32_t          ShownShape     = 0;     // what the rows were built for: type, modes, shape
    ParticleGradientEdit* Gradient   = nullptr;   // the over-life key editors, when shown
    ParticleCurveEdit*    Curve      = nullptr;
    bool              bRebuildQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
