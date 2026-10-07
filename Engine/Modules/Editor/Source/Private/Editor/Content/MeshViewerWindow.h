#ifndef NIMBLEEDITOR_MESHVIEWERWINDOW_H
#define NIMBLEEDITOR_MESHVIEWERWINDOW_H

#if WITH_EDITOR

#include <memory>

#include <QMainWindow>
#include <QStringList>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"

class AnimPreviewWidget;
class MeshPreviewView;
class ClipTimeline;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class QWidget;

/*
 * A .nmmesh, opened from the Content Browser: drawn by the viewport's renderer
 * (MeshPreviewView), its statistics, and a material per section. Picking or
 * dropping a material previews it at once; Save writes them into the mesh as
 * its sections' own materials -- what a RenderComponent draws a section with
 * when its slot is empty -- and restages it, so the level shows them. The
 * engine's meshes preview only: their files are read-only here.
 *
 * A skinned mesh also gets an Animation tab (ANIMATION.md): any clip on its
 * skeleton, or any anim state, played on it -- with the skeleton drawn over
 * it, an anim state's parameters to drive, and a clip's notifies, looping
 * and root motion to edit and save -- and a Skeleton tab, its bones.
 *
 * One window per mesh; opening it again brings that window forward.
 */
class MeshViewerWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /* Shows InPath's viewer, making it if need be; nullptr (warned) if the
     * file is not a mesh this editor can read. */
    static MeshViewerWindow* Open(const QString& InPath, QWidget* InParent);

    const QString& GetPath() const { return Path; }

    /* An .nmanim or .nmanimstate shown on a skinned mesh of its skeleton --
     * found in the project, else asked for. nullptr if there is none. */
    static MeshViewerWindow* OpenAnimation(const QString& InPath, QWidget* InParent);

    /* Plays InRuntimePath (an .nmanim or .nmanimstate); "" holds the reference pose. */
    void SelectAnimation(const QString& InRuntimePath);

    /* The viewer opened (or raised) last, for scripts and tests; null if none. */
    static MeshViewerWindow* FindFront();

    /* A skinned mesh's animated preview; null for a static mesh. */
    AnimPreviewWidget* GetAnimPreview() const { return Anim; }

protected:
    void closeEvent(QCloseEvent* InEvent) override;

private:
    explicit MeshViewerWindow(const QString& InPath);

    bool Load(QString& OutError);
    void BuildUi();
    void FillSections();
    void UpdatePreview();
    void UpdateState();
    bool Save();
    void Revert();

    /* What a section draws with now: its edit, else DefaultLit. */
    QStringList PreviewSections() const;

    /* The Animation and Skeleton tabs, for a skinned mesh. */
    QWidget* BuildAnimationTab();
    QWidget* BuildSkeletonTab();
    void     FillParameters();
    void     FillNotifies();
    void     UpdateAnimationState();
    bool     SaveClip();

    /* The clip as edited, played again where it was: after a change to it. */
    void     ReplayClip();

    QString               Path;          // absolute, on disk
    QString               RuntimePath;   // as a RenderComponent names it
    bool                  bReadOnly = false;
    RenderCore::MeshData  Mesh;
    QStringList           Saved;         // each section's material, as on disk
    QStringList           Edited;        // as picked here

    MeshPreviewView*   Preview = nullptr;
    AnimPreviewWidget* Anim    = nullptr;   // a skinned mesh's; Preview is its view

    /* What the Animation tab plays: a clip -- editable, as on disk -- or an anim state. */
    QString                                        AnimationPath;   // runtime
    QString                                        ClipFile;        // the clip's file on disk; "" for an anim state
    RenderCore::AnimClipData                       ClipEdit;
    bool                                           bClipDirty = false;
    std::shared_ptr<const RenderCore::AnimStateData> State;

    QComboBox*    AnimationPicker = nullptr;
    QLabel*       StateLabel      = nullptr;
    QWidget*      ParameterBox    = nullptr;
    QFormLayout*  ParameterForm   = nullptr;
    std::vector<AnimParameterControl> ParameterControls;
    QWidget*      ClipBox         = nullptr;
    QCheckBox*    ClipLoop        = nullptr;
    QComboBox*    ClipRootMotion  = nullptr;
    QComboBox*    ClipRootBone    = nullptr;   // whose travel root motion is
    ClipTimeline* Timeline        = nullptr;
    QPushButton*  CropButton      = nullptr;
    QTableWidget* NotifyTable     = nullptr;
    QPushButton*  SaveClipButton  = nullptr;
    QLabel*       NotifyFlash     = nullptr;
    QLabel*          Info     = nullptr;
    QLabel*          Status   = nullptr;
    QTableWidget*    Table    = nullptr;
    QPushButton*     SaveButton   = nullptr;
    QPushButton*     RevertButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_MESHVIEWERWINDOW_H
