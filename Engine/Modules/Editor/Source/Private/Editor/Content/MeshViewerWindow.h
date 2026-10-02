#ifndef NIMBLEEDITOR_MESHVIEWERWINDOW_H
#define NIMBLEEDITOR_MESHVIEWERWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QStringList>

#include "Public/RenderCore/MeshAsset.h"

class MeshPreviewView;
class QLabel;
class QPushButton;
class QTableWidget;

/*
 * A .nmmesh, opened from the Content Browser: drawn by the viewport's renderer
 * (MeshPreviewView), its statistics, and a material per section. Picking or
 * dropping a material previews it at once; Save writes them into the mesh as
 * its sections' own materials -- what a RenderComponent draws a section with
 * when its slot is empty -- and restages it, so the level shows them. The
 * engine's meshes preview only: their files are read-only here.
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

    QString               Path;          // absolute, on disk
    QString               RuntimePath;   // as a RenderComponent names it
    bool                  bReadOnly = false;
    RenderCore::MeshData  Mesh;
    QStringList           Saved;         // each section's material, as on disk
    QStringList           Edited;        // as picked here

    MeshPreviewView* Preview  = nullptr;
    QLabel*          Info     = nullptr;
    QLabel*          Status   = nullptr;
    QTableWidget*    Table    = nullptr;
    QPushButton*     SaveButton   = nullptr;
    QPushButton*     RevertButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_MESHVIEWERWINDOW_H
