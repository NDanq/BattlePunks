#ifndef NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H
#define NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QStringList>

#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/MeshAsset.h"

class AnimPreviewWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

/*
 * An .nmanimstate, edited as forms (ANIMATION.md): its parameters; its
 * layers, each with a weight, a weight parameter and a bone mask; each
 * layer's states -- a clip, or a blend space of clips along a parameter --
 * and the transitions between them, with their conditions. Beside them, a
 * skinned mesh on the clips' skeleton plays it live, its parameters to drive
 * by hand. Save writes the file and restages it, so levels pick it up.
 *
 * A parameter renamed here is renamed where it is used; a state removed
 * takes its transitions with it. One window per file.
 */
class AnimStateEditorWindow final : public QMainWindow
{
    Q_OBJECT

public:
    static AnimStateEditorWindow* Open(const QString& InPath, QWidget* InParent);

    /* The editor opened (or raised) last, for scripts and tests; null if none. */
    static AnimStateEditorWindow* FindFront();

    const QString&     GetPath() const    { return Path; }
    AnimPreviewWidget* GetPreview() const { return Preview; }
    bool               Save();

protected:
    void closeEvent(QCloseEvent* InEvent) override;

private:
    explicit AnimStateEditorWindow(const QString& InPath);

    bool Load(QString& OutError);
    void BuildUi();
    QWidget* BuildParameters();
    QWidget* BuildLayers();
    QWidget* BuildStates();
    QWidget* BuildTransitions();
    QWidget* BuildPreview();

    /* After any edit: dirty, checked, previewed. bInStructure: names or
     * counts changed, so the lists and pickers are refilled too. */
    void Edited(bool bInStructure = false, bool bInDirty = true);

    void FillAll();
    void FillParameters();
    void FillLayers();
    void FillLayer();
    void FillStates();
    void FillState();
    void FillTransitions();
    void FillTransition();
    void FillLive();
    void UpdateLive();
    void UpdateTitle();
    void PickMesh(const QString& InFile);
    void RestartPreview();

    RenderCore::AnimLayer*      CurrentLayer();
    RenderCore::AnimStateNode*  CurrentState();
    RenderCore::AnimTransition* CurrentTransition();
    QStringList ParameterNames(bool bInFloatsOnly) const;
    QString     DescribeTransition(const RenderCore::AnimLayer& InLayer, const RenderCore::AnimTransition& InTransition) const;

    /* InOld renamed InNew wherever a layer, state or condition names it. */
    void RenameParameter(const std::string& InOld, const std::string& InNew);

    QString                   Path;
    bool                      bReadOnly = false;
    RenderCore::AnimStateData Saved;
    RenderCore::AnimStateData Edit;
    bool                      bFilling = false;
    bool                      bDirty   = false;

    /* The preview's mesh: found by the clips' skeleton, or picked. */
    QString                   MeshRuntime;
    RenderCore::MeshData      PreviewMesh;

    QTableWidget*   ParameterTable = nullptr;

    QListWidget*    LayerList   = nullptr;
    QLineEdit*      LayerName   = nullptr;
    QDoubleSpinBox* LayerWeight = nullptr;
    QComboBox*      LayerWeightParameter = nullptr;
    QLineEdit*      LayerMask   = nullptr;
    QComboBox*      LayerEntry  = nullptr;

    QListWidget*    StateList   = nullptr;
    QWidget*        StateForm   = nullptr;
    QLineEdit*      StateName   = nullptr;
    QDoubleSpinBox* StateSpeed  = nullptr;
    QComboBox*      StateLoop   = nullptr;
    QComboBox*      StateBlend  = nullptr;
    QTableWidget*   SampleTable = nullptr;

    QListWidget*    TransitionList = nullptr;
    QWidget*        TransitionForm = nullptr;
    QComboBox*      TransitionFrom = nullptr;
    QComboBox*      TransitionTo   = nullptr;
    QDoubleSpinBox* TransitionDuration = nullptr;
    QCheckBox*      TransitionAtEnd    = nullptr;
    QTableWidget*   ConditionTable     = nullptr;

    AnimPreviewWidget* Preview     = nullptr;
    QLabel*            MeshLabel   = nullptr;
    QGroupBox*         LiveBox     = nullptr;
    QFormLayout*       LiveForm    = nullptr;
    QLabel*            LiveStates  = nullptr;
    QLabel*            Problems    = nullptr;
    QPushButton*       SaveButton  = nullptr;
    QPushButton*       RevertButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H
