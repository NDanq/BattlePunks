#ifndef NIMBLEEDITOR_WORLDSETTINGSDIALOG_H
#define NIMBLEEDITOR_WORLDSETTINGSDIALOG_H

#if WITH_EDITOR

#include <QDialog>

class EditorDocument;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

/*
 * World > World Settings: the edited world's WorldSettings (GameCore's
 * WorldSettings.h) -- its game mode and baked lighting. OK applies them through
 * EditorDocument::SetWorldSettings, one undo step, saved with the world.
 */
class WorldSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    WorldSettingsDialog(EditorDocument& InDocument, QWidget* InParent = nullptr);

private:
    void Apply();

    EditorDocument& Document;

    QComboBox*      DetailMode      = nullptr;   // WORLD_DETAIL_USE_GLOBAL or a tier
    QComboBox*      GameMode        = nullptr;   // editable: a mode the loaded module lacks can still be named
    QCheckBox*      BakedLighting   = nullptr;
    QDoubleSpinBox* TexelsPerMetre  = nullptr;   // shown per metre; stored per unit
    QSpinBox*       Bounces         = nullptr;
    QSpinBox*       SamplesPerTexel = nullptr;
    QDoubleSpinBox* ProbeSpacing    = nullptr;
    QComboBox*      MaxAtlasSize    = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_WORLDSETTINGSDIALOG_H
