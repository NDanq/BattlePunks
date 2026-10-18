#ifndef NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H
#define NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QStringList>

#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"

class AnimPreviewWidget;
class AnimStateGraph;
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
 * An .nmanimstate (ANIMATION.md): its parameters and layers as forms; the
 * selected layer's states and transitions as a graph (AnimStateGraph), what
 * is picked there edited in a form beside it -- a state's clip or blend
 * space, a transition's crossfade and conditions. Beside them, a skinned
 * mesh on the clips' skeleton plays it live, its parameters to drive by
 * hand, the state it is in lit on the graph. Save writes the file and
 * restages it, so levels pick it up.
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
    QWidget* BuildLookAt();
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
    void FillLookAt();
    void FillStates();
    void FillState();
    void FillTransitions();
    void FillTransition();
    void FillLive();

    /* The graph: the current layer redrawn (laid out first if no one has
     * placed it), with what the forms have selected. */
    void SyncGraph();
    /* The state form, or the transition form, beside the graph. */
    void ShowDetail(bool bInTransition);

    /* The graph's (and the hidden lists') edits to the current layer. */
    void AddState(const QPointF& InAt);
    void RemoveState(int InState);
    void AddTransition(int InFrom, int InTo);
    void RemoveTransition(int InTransition);
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

    QLineEdit*      RetargetField  = nullptr;   // Edit.Retarget
    QTableWidget*   ParameterTable = nullptr;

    QListWidget*    LayerList   = nullptr;
    QLineEdit*      LayerName   = nullptr;
    QDoubleSpinBox* LayerWeight = nullptr;
    QComboBox*      LayerWeightParameter = nullptr;
    QLineEdit*      LayerMask   = nullptr;
    QComboBox*      LayerEntry  = nullptr;
    QComboBox*      LayerBlend  = nullptr;   // override or additive
    QComboBox*      LayerBase   = nullptr;   // an additive layer's base

    QComboBox*      LookParameter = nullptr;
    QComboBox*      LookWeightParameter = nullptr;
    QDoubleSpinBox* LookMin      = nullptr;
    QDoubleSpinBox* LookMax      = nullptr;
    QDoubleSpinBox* LookAxis[3]  = { nullptr, nullptr, nullptr };
    QDoubleSpinBox* LookResponse = nullptr;
    QLineEdit*      LookBones    = nullptr;   // "bone:share, bone:share"

    AnimStateGraph* Graph       = nullptr;
    QGroupBox*      StateBox    = nullptr;
    QGroupBox*      TransitionBox = nullptr;

    QListWidget*    StateList   = nullptr;   // hidden: the selection model behind the graph
    QWidget*        StateForm   = nullptr;
    QLineEdit*      StateName   = nullptr;
    QDoubleSpinBox* StateSpeed  = nullptr;
    QComboBox*      StateLoop   = nullptr;
    QLineEdit*      StateSync   = nullptr;
    QComboBox*      StateBlend  = nullptr;
    QComboBox*      StateBlendY = nullptr;   // a second: a 2D blend space
    QTableWidget*   SampleTable = nullptr;

    QListWidget*    TransitionList = nullptr;   // hidden, likewise
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
    std::vector<AnimParameterControl> LiveControls;
    QLabel*            LiveStates  = nullptr;
    QLabel*            Problems    = nullptr;
    QPushButton*       SaveButton  = nullptr;
    QPushButton*       RevertButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ANIMSTATEEDITORWINDOW_H
