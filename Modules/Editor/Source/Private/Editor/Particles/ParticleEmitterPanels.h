#ifndef NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
#define NIMBLEEDITOR_PARTICLEEMITTERPANELS_H

#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QWidget>

class ParticleDocument;
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
 * Rendering, Emission, Motion, Beam and Over Life, each with its reset
 * arrow -- only those its type and spawn mode use.
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

    ParticleDocument& Document;
    QWidget*          Content = nullptr;
    QVBoxLayout*      ContentLayout = nullptr;
    QLabel*           Budget  = nullptr;
    std::vector<Row>  Rows;
    uint8_t           ShownType      = 0;     // what the rows were built for
    uint8_t           ShownSpawnMode = 0;
    bool              bRebuildQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEEMITTERPANELS_H
