#ifndef NIMBLEEDITOR_RETARGETEDITORWINDOW_H
#define NIMBLEEDITOR_RETARGETEDITORWINDOW_H

#if WITH_EDITOR

#include <memory>
#include <vector>

#include <QMainWindow>
#include <QStringList>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RetargetAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"

class AnimPreviewWidget;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

/*
 * An .nmretarget (ANIMATION.md, "Retargeting"): its target skeleton, and for
 * each source skeleton a table of the target's bones -- which source bone
 * drives each, what its translation does, whether its pose is offset --
 * filled by Auto Map, its arms by Align Arms, or by hand. Beside it, a clip
 * of the source plays on a source mesh and, through the retarget as it is
 * being edited, on a target mesh, in step; the bones no source bone drives
 * are drawn red. Bake Clips writes chosen clips out as ordinary clips on the
 * target. Save writes the file and restages it; Ctrl+Z undoes. One window
 * per file.
 */
class RetargetEditorWindow final : public QMainWindow
{
    Q_OBJECT

public:
    static RetargetEditorWindow* Open(const QString& InPath, QWidget* InParent);

    /* Asks for a name (and, when InTargetSkeleton is empty, the target) and
     * writes an empty retarget in InFolder (the project's Content when
     * empty); opened if bInOpen. Its path, or "" if cancelled. */
    static QString CreateIn(const QString& InFolder, const QString& InTargetSkeleton, QWidget* InParent, bool bInOpen = true);

    /* The editor opened (or raised) last, for scripts and tests; null if none. */
    static RetargetEditorWindow* FindFront();

    const QString&                  GetPath() const { return Path; }
    const RenderCore::RetargetData& GetData() const { return Edit; }
    AnimPreviewWidget*              GetTargetPreview() const { return TargetPreview; }
    bool                            Save();

    /* What the window's controls do, for scripts and tests. Each edits the
     * selected source; false (nothing changed) if there is none. */
    bool   AddSource(const QString& InSkeleton);
    void   SelectSource(int InIndex);
    size_t AutoMap();
    bool   AlignArms();
    void   ClearMap();
    void   Undo();
    void   Redo();
    /* Plays InClipRuntime (a clip of the selected source) in both previews. */
    bool   PreviewClip(const QString& InClipRuntime);
    /* Each of InClipFiles baked through the saved retarget into InFolder as
     * <name><InSuffix>.nmanim; the files written. Saves first if need be. */
    QStringList BakeClips(const QStringList& InClipFiles, const QString& InFolder, const QString& InSuffix, QStringList& OutErrors);

protected:
    void closeEvent(QCloseEvent* InEvent) override;

private:
    explicit RetargetEditorWindow(const QString& InPath);

    bool     Load(QString& OutError);
    void     BuildUi();
    QWidget* BuildMap();
    QWidget* BuildPreview();

    /* Before an edit: the undo point. After it: dirty, refilled, previewed. */
    void Remember();
    void Edited(bool bInStructure);
    void FillAll();
    void FillSources();
    void FillBones();
    void FillClips();
    void UpdateMarks();
    void UpdateTitle();
    void LoadSkeletons();
    void RestartPreview();
    void ShowBakeDialog();

    RenderCore::RetargetSource* CurrentSource();
    /* The selected source's entry for target bone InTarget, made if need be. */
    RenderCore::RetargetBoneMap& MapFor(const std::string& InTarget);

    QString                  Path;
    bool                     bReadOnly = false;
    RenderCore::RetargetData Saved;
    RenderCore::RetargetData Edit;
    std::vector<RenderCore::RetargetData> UndoStack, RedoStack;
    int                      Current  = -1;   // the selected source
    bool                     bFilling = false;
    bool                     bDirty   = false;

    std::shared_ptr<const RenderCore::SkeletonData> TargetSkeleton;
    std::shared_ptr<const RenderCore::SkeletonData> SourceSkeleton;   // the selected source's
    QString              SourceMeshRuntime, TargetMeshRuntime;
    RenderCore::MeshData SourceMesh, TargetMesh;
    QString              ClipRuntime;                                   // what the previews play

    QLineEdit*      TargetField  = nullptr;
    QListWidget*    SourceList   = nullptr;
    QDoubleSpinBox* RootScale    = nullptr;
    QLabel*         RootScaleAuto = nullptr;
    QTableWidget*   BoneTable    = nullptr;
    QLabel*         MapSummary   = nullptr;
    QComboBox*      ClipPicker   = nullptr;
    QLabel*         SourceLabel  = nullptr;
    QLabel*         TargetLabel  = nullptr;
    AnimPreviewWidget* SourcePreview = nullptr;
    AnimPreviewWidget* TargetPreview = nullptr;
    QPushButton*    SaveButton   = nullptr;
    QPushButton*    RevertButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_RETARGETEDITORWINDOW_H
