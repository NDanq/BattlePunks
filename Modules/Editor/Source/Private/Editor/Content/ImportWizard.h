#ifndef NIMBLEEDITOR_IMPORTWIZARD_H
#define NIMBLEEDITOR_IMPORTWIZARD_H

#if WITH_EDITOR

#include <QStringList>
#include <QWizard>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QTableWidget;

/*
 * Files from outside the project -- dropped on the Content Browser or picked
 * with File > Import -- converted into engine assets (AssetImport.h):
 *
 *   Sources           each file, what it is, the name it gets; the folder
 *   Mesh Options      only with a mesh: scale, axes, normals, split, textures
 *   Texture Options   only with an image (or a mesh's textures to import)
 *   Import            the conversions, on a worker thread, with a log
 *
 * The options start from the [Editor] Import* settings and are saved back to
 * them, so the next import offers what was used last. Imported() lists every
 * file written, each already staged beside the executable.
 *
 * Modeless; deletes itself on close.
 */
class ImportWizard : public QWizard
{
    Q_OBJECT

public:
    ImportWizard(const QStringList& InFiles, const QString& InDestFolder, QWidget* InParent);

    /* Not while an import runs: it would finish writing with no one listening. */
    void reject() override;

signals:
    void Imported(const QStringList& InWritten);

private:
    enum EPage { Page_Sources, Page_Mesh, Page_Texture, Page_Import };

    int  nextId() const override;
    void initializePage(int InId) override;

    QWizardPage* BuildSourcesPage();
    QWizardPage* BuildMeshPage();
    QWizardPage* BuildTexturePage();
    QWizardPage* BuildImportPage();

    bool HasChecked(int InKind) const;
    void RefreshStatus();
    void ListReferencedImages();
    void SaveOptions();
    void StartImport();
    void OnJobDone(const QString& InLog, const QStringList& InWritten, bool bInOK);

    QStringList Files;

    QTableWidget*   SourceTable   = nullptr;
    QLineEdit*      DestFolder    = nullptr;

    QDoubleSpinBox* MeshScale     = nullptr;
    QCheckBox*      ConvertAxes   = nullptr;
    QCheckBox*      GenNormals    = nullptr;
    QCheckBox*      Combine       = nullptr;
    QCheckBox*      MeshTextures  = nullptr;
    QListWidget*    ImageList     = nullptr;

    QCheckBox*      SRGB          = nullptr;
    QComboBox*      Filter        = nullptr;
    QComboBox*      Wrap          = nullptr;
    QComboBox*      MaxSize       = nullptr;
    QCheckBox*      FlipVertical  = nullptr;

    QProgressBar*   Progress      = nullptr;
    QPlainTextEdit* Log           = nullptr;

    QStringList Written;
    int  JobsLeft  = 0;
    int  Failures  = 0;
    bool bRunning  = false;
    bool bFinished = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_IMPORTWIZARD_H
