#ifndef NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
#define NIMBLEEDITOR_PARTICLEEMITTERPANELS_H

#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QSet>
#include <QString>
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
 * the Details panel's own property widgets (PropertyWidgets.h), in
 * sections drawn as the Details panel draws components: collapsible, with
 * Reset, each row with its reset arrow -- only the rows its type, modes and
 * shape use -- and a colour gradient and size curve editor
 * (ParticleCurveWidgets.h) when chosen. Rendering, Emission, Size and Color
 * (and a beam's Beam) are always there; Shape, Velocity, Forces and Rotation
 * are modules (EParticleModule), removed with their x and added back with
 * "+ Add Module", as components are.
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

    struct SectionState
    {
        QString      Name;
        QToolButton* ResetAll = nullptr;
    };

    void Rebuild();
    void Refresh();

    /* A section as the Details panel draws a component: a bold header that
     * collapses it, Reset, and -- an optional module's -- a remove button;
     * then the form its rows go in (OutBody, their parent). */
    QFormLayout* AddSection(const char* InName, uint8_t InModule, const QString& InTip, QWidget*& OutBody);

    /* "+ Add Module": the optional modules the emitter's type uses and it lacks. */
    QWidget* BuildAddModuleButton();

    /* A gradient's (or curve's) row: its label, then the editor across the form. */
    void AddKeyEditor(bool bInGradient, const QString& InName, const QString& InTip, QFormLayout* InForm);

    ParticleDocument& Document;
    QWidget*          Content = nullptr;
    QVBoxLayout*      ContentLayout = nullptr;
    QLabel*           Budget  = nullptr;
    std::vector<Row>  Rows;
    std::vector<SectionState> Sections;
    QSet<QString>     CollapsedSections;     // by name, kept across rebuilds and emitters
    uint32_t          ShownShape     = 0;     // what the rows were built for: type, modes, shape
    ParticleGradientEdit* Gradient   = nullptr;   // the over-life key editors, when shown
    ParticleCurveEdit*    Curve      = nullptr;
    bool              bRebuildQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
