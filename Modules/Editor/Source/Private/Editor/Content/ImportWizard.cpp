#if WITH_EDITOR

#include "Private/Editor/Content/ImportWizard.h"

#include <functional>
#include <memory>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/SettingsDialog.h"

/* What the wizard starts with -- the last import's choices (Edit > Editor
 * Settings > Import shows them too). */
NIMBLE_CONFIG_FLOAT(GEditorImportMeshScale,          "Editor", "ImportMeshScale",          100.0f, nullptr);
NIMBLE_CONFIG_BOOL (GEditorImportConvertCoordinates, "Editor", "ImportConvertCoordinates", true,   nullptr);
NIMBLE_CONFIG_BOOL (GEditorImportGenerateNormals,    "Editor", "ImportGenerateNormals",    true,   nullptr);
NIMBLE_CONFIG_BOOL (GEditorImportCombineMeshes,      "Editor", "ImportCombineMeshes",      true,   nullptr);
NIMBLE_CONFIG_BOOL (GEditorImportMeshTextures,       "Editor", "ImportMeshTextures",       true,   nullptr);
NIMBLE_CONFIG_BOOL (GEditorImportTextureSRGB,        "Editor", "ImportTextureSRGB",        true,   nullptr);
NIMBLE_CONFIG_INT  (GEditorImportTextureMaxSize,     "Editor", "ImportTextureMaxSize",     4096,   nullptr);

using namespace EditorContent;

namespace
{
    enum EColumn { Col_File, Col_Kind, Col_Name, Col_Status, Col_Count };

    /* A page whose Next / Finish waits on a condition the wizard decides. */
    class ConditionPage final : public QWizardPage
    {
    public:
        explicit ConditionPage(std::function<bool()> InComplete) : Complete(std::move(InComplete)) {}
        bool isComplete() const override { return Complete ? Complete() : true; }
        void Recheck() { emit completeChanged(); }

    private:
        std::function<bool()> Complete;
    };

    /* One file's conversion, everything it needs copied in: it runs on a worker. */
    struct Job
    {
        EImportKind          Kind = EImportKind::None;
        QString              Source;
        QString              BaseName;
        QString              DestFolder;
        QString              ProjectRoot;
        MeshImportOptions    Mesh;
        TextureImportOptions Texture;
    };

    QString KindText(const EImportKind InKind)
    {
        switch(InKind)
        {
            case EImportKind::Mesh:    return QStringLiteral("Mesh");
            case EImportKind::Texture: return QStringLiteral("Texture");
            case EImportKind::None:    break;
        }
        return QStringLiteral("Not importable");
    }
}

ImportWizard::ImportWizard(const QStringList& InFiles, const QString& InDestFolder, QWidget* InParent)
    : QWizard(InParent)
    , Files(InFiles)
{
    setWindowTitle(QStringLiteral("Import"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnLastPage, true);
    setOption(QWizard::NoCancelButtonOnLastPage, true);
    resize(760, 520);

    setPage(Page_Sources, BuildSourcesPage());
    setPage(Page_Mesh,    BuildMeshPage());
    setPage(Page_Texture, BuildTexturePage());
    setPage(Page_Import,  BuildImportPage());
    setStartId(Page_Sources);

    DestFolder->setText(QDir::toNativeSeparators(InDestFolder));
    RefreshStatus();

    /* The button that starts the work says so. */
    connect(this, &QWizard::currentIdChanged, this, [this](const int)
    {
        setButtonText(QWizard::NextButton, nextId() == Page_Import ? QStringLiteral("Import") : QStringLiteral("Next >"));
    });
}

void ImportWizard::reject()
{
    if(bRunning)
    {
        return;
    }
    QWizard::reject();
}

// === Pages ===========================================================================

QWizardPage* ImportWizard::BuildSourcesPage()
{
    auto* Page = new ConditionPage([this]()
    {
        return !DestFolder->text().trimmed().isEmpty()
            && (HasChecked(int(EImportKind::Mesh)) || HasChecked(int(EImportKind::Texture)));
    });
    Page->setTitle(QStringLiteral("Files to import"));
    Page->setSubTitle(QStringLiteral("Meshes become .nmmesh and images .nmtexture, in the folder below. "
                                     "Untick a file to leave it out; edit a name to rename its asset."));

    SourceTable = new QTableWidget(int(Files.size()), Col_Count, Page);
    SourceTable->setHorizontalHeaderLabels({ QStringLiteral("File"), QStringLiteral("Kind"),
                                             QStringLiteral("Asset name"), QStringLiteral("") });
    SourceTable->horizontalHeader()->setSectionResizeMode(Col_File, QHeaderView::Stretch);
    SourceTable->horizontalHeader()->setSectionResizeMode(Col_Name, QHeaderView::Stretch);
    SourceTable->verticalHeader()->hide();
    SourceTable->setSelectionMode(QAbstractItemView::NoSelection);

    for(int Row = 0; Row < Files.size(); ++Row)
    {
        const QString& Path = Files[Row];
        const EImportKind Kind = ClassifySource(Path);

        auto* File = new QTableWidgetItem(QFileInfo(Path).fileName());
        File->setToolTip(QDir::toNativeSeparators(Path));
        File->setFlags(Qt::ItemIsEnabled | (Kind != EImportKind::None ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
        File->setCheckState(Kind != EImportKind::None ? Qt::Checked : Qt::Unchecked);
        File->setData(Qt::UserRole, int(Kind));
        SourceTable->setItem(Row, Col_File, File);

        auto* KindItem = new QTableWidgetItem(KindText(Kind));
        KindItem->setFlags(Qt::ItemIsEnabled);
        SourceTable->setItem(Row, Col_Kind, KindItem);

        auto* Name = new QTableWidgetItem(QFileInfo(Path).completeBaseName());
        Name->setFlags(Kind != EImportKind::None ? (Qt::ItemIsEnabled | Qt::ItemIsEditable) : Qt::NoItemFlags);
        SourceTable->setItem(Row, Col_Name, Name);

        auto* Status = new QTableWidgetItem();
        Status->setFlags(Qt::ItemIsEnabled);
        SourceTable->setItem(Row, Col_Status, Status);
    }
    connect(SourceTable, &QTableWidget::itemChanged, this, [this, Page](QTableWidgetItem*)
    {
        RefreshStatus();
        Page->Recheck();
    });

    DestFolder = new QLineEdit(Page);
    auto* Browse = new QPushButton(QStringLiteral("Browse..."), Page);
    connect(DestFolder, &QLineEdit::textChanged, this, [this, Page]()
    {
        RefreshStatus();
        Page->Recheck();
    });
    connect(Browse, &QPushButton::clicked, this, [this]()
    {
        const QString Chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Import Into"), DestFolder->text());
        if(!Chosen.isEmpty()) { DestFolder->setText(QDir::toNativeSeparators(Chosen)); }
    });

    auto* DestRow = new QHBoxLayout();
    DestRow->addWidget(new QLabel(QStringLiteral("Import into:"), Page));
    DestRow->addWidget(DestFolder, 1);
    DestRow->addWidget(Browse);

    auto* Layout = new QVBoxLayout(Page);
    Layout->addWidget(SourceTable, 1);
    Layout->addLayout(DestRow);
    return Page;
}

QWizardPage* ImportWizard::BuildMeshPage()
{
    auto* Page = new QWizardPage();
    Page->setTitle(QStringLiteral("Mesh options"));
    Page->setSubTitle(QStringLiteral("For every glTF / GLB file. Edit > Editor Settings > Import keeps the defaults."));

    MeshScale = new QDoubleSpinBox(Page);
    MeshScale->setRange(0.0001, 100000.0);
    MeshScale->setDecimals(4);
    MeshScale->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    MeshScale->setValue(double(GEditorImportMeshScale.GetFloat()));
    MeshScale->setToolTip(QStringLiteral("Multiplies every position. glTF is in metres: 100 makes a metre 100 units"));

    ConvertAxes = new QCheckBox(QStringLiteral("Convert glTF axes to the engine's (left-handed, +Y up, facing the camera)"), Page);
    ConvertAxes->setChecked(GEditorImportConvertCoordinates.GetBool());

    GenNormals = new QCheckBox(QStringLiteral("Generate smooth normals where the file has none"), Page);
    GenNormals->setChecked(GEditorImportGenerateNormals.GetBool());

    Combine = new QCheckBox(QStringLiteral("Combine the scene into one mesh (off: one .nmmesh per glTF mesh)"), Page);
    Combine->setChecked(GEditorImportCombineMeshes.GetBool());

    MeshTextures = new QCheckBox(QStringLiteral("Import the textures it uses, beside it"), Page);
    MeshTextures->setChecked(GEditorImportMeshTextures.GetBool());

    ImageList = new QListWidget(Page);
    ImageList->setSelectionMode(QAbstractItemView::NoSelection);
    connect(MeshTextures, &QCheckBox::toggled, ImageList, &QWidget::setEnabled);
    ImageList->setEnabled(MeshTextures->isChecked());

    auto* Form = new QFormLayout(Page);
    Form->addRow(QStringLiteral("Scale"), MeshScale);
    Form->addRow(QString(), ConvertAxes);
    Form->addRow(QString(), GenNormals);
    Form->addRow(QString(), Combine);
    Form->addRow(QString(), MeshTextures);
    Form->addRow(QStringLiteral("Textures used"), ImageList);
    return Page;
}

QWizardPage* ImportWizard::BuildTexturePage()
{
    auto* Page = new QWizardPage();
    Page->setTitle(QStringLiteral("Texture options"));
    Page->setSubTitle(QStringLiteral("For every image, and every texture a mesh brings with it."));

    SRGB = new QCheckBox(QStringLiteral("Colour data (sRGB) -- off for masks, normal maps and other data"), Page);
    SRGB->setChecked(GEditorImportTextureSRGB.GetBool());

    Filter = new QComboBox(Page);
    Filter->addItem(QStringLiteral("Linear (smooth)"), int(RenderCore::ETextureFilter::Linear));
    Filter->addItem(QStringLiteral("Nearest (pixelated)"), int(RenderCore::ETextureFilter::Nearest));

    Wrap = new QComboBox(Page);
    Wrap->addItem(QStringLiteral("Repeat"), int(RenderCore::ETextureWrap::Repeat));
    Wrap->addItem(QStringLiteral("Clamp to edge"), int(RenderCore::ETextureWrap::Clamp));
    Wrap->addItem(QStringLiteral("Mirror"), int(RenderCore::ETextureWrap::Mirror));

    MaxSize = new QComboBox(Page);
    for(const int Size : { 256, 512, 1024, 2048, 4096, 8192, 16384 }) { MaxSize->addItem(QString::number(Size), Size); }
    const int Saved = MaxSize->findData(GEditorImportTextureMaxSize.GetInt());
    MaxSize->setCurrentIndex(Saved >= 0 ? Saved : MaxSize->findData(4096));
    MaxSize->setToolTip(QStringLiteral("A larger image is scaled down to fit, keeping its shape"));

    FlipVertical = new QCheckBox(QStringLiteral("Flip vertically"), Page);

    auto* Form = new QFormLayout(Page);
    Form->addRow(QString(), SRGB);
    Form->addRow(QStringLiteral("Filtering"), Filter);
    Form->addRow(QStringLiteral("Wrapping"), Wrap);
    Form->addRow(QStringLiteral("Largest size"), MaxSize);
    Form->addRow(QString(), FlipVertical);
    return Page;
}

QWizardPage* ImportWizard::BuildImportPage()
{
    auto* Page = new ConditionPage([this]() { return bFinished; });
    Page->setTitle(QStringLiteral("Importing"));
    Page->setFinalPage(true);

    Progress = new QProgressBar(Page);
    Log = new QPlainTextEdit(Page);
    Log->setReadOnly(true);

    auto* Layout = new QVBoxLayout(Page);
    Layout->addWidget(Progress);
    Layout->addWidget(Log, 1);
    return Page;
}

// === Flow ============================================================================

bool ImportWizard::HasChecked(const int InKind) const
{
    for(int Row = 0; Row < SourceTable->rowCount(); ++Row)
    {
        const QTableWidgetItem* File = SourceTable->item(Row, Col_File);
        if(File && File->checkState() == Qt::Checked && File->data(Qt::UserRole).toInt() == InKind) { return true; }
    }
    return false;
}

int ImportWizard::nextId() const
{
    const bool bMeshes   = HasChecked(int(EImportKind::Mesh));
    const bool bTextures = HasChecked(int(EImportKind::Texture)) || (bMeshes && MeshTextures->isChecked());
    switch(currentId())
    {
        case Page_Sources: return bMeshes ? Page_Mesh : (bTextures ? Page_Texture : Page_Import);
        case Page_Mesh:    return bTextures ? Page_Texture : Page_Import;
        case Page_Texture: return Page_Import;
        default:           return -1;
    }
}

void ImportWizard::initializePage(const int InId)
{
    if(InId == Page_Mesh)    { ListReferencedImages(); }
    if(InId == Page_Import)  { StartImport(); }
    QWizard::initializePage(InId);
}

/* "will replace Crate.nmmesh" beside a row whose asset already exists. */
void ImportWizard::RefreshStatus()
{
    if(!SourceTable || !DestFolder) { return; }
    const QSignalBlocker Block(SourceTable);
    const QDir Dest(QDir::fromNativeSeparators(DestFolder->text().trimmed()));
    for(int Row = 0; Row < SourceTable->rowCount(); ++Row)
    {
        const auto Kind = EImportKind(SourceTable->item(Row, Col_File)->data(Qt::UserRole).toInt());
        QTableWidgetItem* Status = SourceTable->item(Row, Col_Status);
        if(Kind == EImportKind::None)
        {
            Status->setText(QStringLiteral("skipped"));
            continue;
        }
        const QString Asset = SourceTable->item(Row, Col_Name)->text().trimmed() + GetAssetExtension(Kind);
        Status->setText(Dest.exists(Asset) ? QStringLiteral("will replace %1").arg(Asset) : QString());
        Status->setForeground(Dest.exists(Asset) ? QBrush(QColor(220, 140, 40)) : QBrush());
    }
}

void ImportWizard::ListReferencedImages()
{
    ImageList->clear();
    for(int Row = 0; Row < SourceTable->rowCount(); ++Row)
    {
        const QTableWidgetItem* File = SourceTable->item(Row, Col_File);
        if(File->checkState() != Qt::Checked || File->data(Qt::UserRole).toInt() != int(EImportKind::Mesh)) { continue; }

        QList<ReferencedImage> Images;
        QString Error;
        if(!FindReferencedImages(Files[Row], Images, Error))
        {
            ImageList->addItem(Error);
            continue;
        }
        for(const ReferencedImage& Image : Images)
        {
            const QString Where = Image.SourcePath.isEmpty() ? QStringLiteral("embedded")
                                                             : QFileInfo(Image.SourcePath).fileName();
            ImageList->addItem(QStringLiteral("%1: %2 (%3)").arg(File->text(), Image.Name, Where));
        }
    }
    if(ImageList->count() == 0) { ImageList->addItem(QStringLiteral("None")); }
}

void ImportWizard::SaveOptions()
{
    GEditorImportMeshScale.SetFloat(float(MeshScale->value()));
    GEditorImportConvertCoordinates.SetBool(ConvertAxes->isChecked());
    GEditorImportGenerateNormals.SetBool(GenNormals->isChecked());
    GEditorImportCombineMeshes.SetBool(Combine->isChecked());
    GEditorImportMeshTextures.SetBool(MeshTextures->isChecked());
    GEditorImportTextureSRGB.SetBool(SRGB->isChecked());
    GEditorImportTextureMaxSize.SetInt(MaxSize->currentData().toInt());
    SettingsDialog::SaveUserSettings();
}

void ImportWizard::StartImport()
{
    SaveOptions();

    MeshImportOptions Mesh;
    Mesh.Scale               = float(MeshScale->value());
    Mesh.bConvertCoordinates = ConvertAxes->isChecked();
    Mesh.bGenerateNormals    = GenNormals->isChecked();
    Mesh.bCombineMeshes      = Combine->isChecked();
    Mesh.bImportTextures     = MeshTextures->isChecked();

    TextureImportOptions Texture;
    Texture.bSRGB           = SRGB->isChecked();
    Texture.Filter          = RenderCore::ETextureFilter(Filter->currentData().toInt());
    Texture.Wrap            = RenderCore::ETextureWrap(Wrap->currentData().toInt());
    Texture.MaxDimension    = MaxSize->currentData().toInt();
    Texture.bFlipVertically = FlipVertical->isChecked();

    auto Jobs = std::make_shared<QList<Job>>();
    const QString Dest = QDir::fromNativeSeparators(DestFolder->text().trimmed());
    const QString Root = EditorGraphs::GetProjectRoot();
    for(int Row = 0; Row < SourceTable->rowCount(); ++Row)
    {
        const QTableWidgetItem* File = SourceTable->item(Row, Col_File);
        if(File->checkState() != Qt::Checked) { continue; }
        Job Each;
        Each.Kind        = EImportKind(File->data(Qt::UserRole).toInt());
        Each.Source      = Files[Row];
        Each.BaseName    = SourceTable->item(Row, Col_Name)->text().trimmed();
        Each.DestFolder  = Dest;
        Each.ProjectRoot = Root;
        Each.Mesh        = Mesh;
        Each.Texture     = Texture;
        if(Each.BaseName.isEmpty()) { Each.BaseName = QFileInfo(Each.Source).completeBaseName(); }
        Jobs->append(Each);
    }

    JobsLeft  = int(Jobs->size());
    Failures  = 0;
    bRunning  = JobsLeft > 0;
    bFinished = !bRunning;
    Progress->setRange(0, qMax(1, JobsLeft));
    Progress->setValue(0);
    Log->clear();
    Log->appendPlainText(QStringLiteral("Importing %1 file(s) into %2").arg(JobsLeft).arg(QDir::toNativeSeparators(Dest)));

    /* One worker, in order: a readable log, and no two jobs writing a shared
     * texture. The wizard can't close until the last result is back. */
    QThreadPool::globalInstance()->start([this, Jobs]()
    {
        for(const Job& Each : *Jobs)
        {
            const ImportResult Result = Each.Kind == EImportKind::Mesh
                ? ImportMesh(Each.Source, Each.DestFolder, Each.BaseName, Each.Mesh, Each.Texture, Each.ProjectRoot)
                : ImportTexture(Each.Source, Each.DestFolder, Each.BaseName, Each.Texture, Each.ProjectRoot);

            QString Text = (Result.bOK ? QStringLiteral("OK    ") : QStringLiteral("FAIL  ")) + Result.Message;
            for(const QString& Warning : Result.Warnings) { Text += QStringLiteral("\n      ") + Warning; }

            const QStringList Out = Result.Written;
            const bool        bOK = Result.bOK;
            QMetaObject::invokeMethod(this, [this, Text, Out, bOK]() { OnJobDone(Text, Out, bOK); }, Qt::QueuedConnection);
        }
    });
}

void ImportWizard::OnJobDone(const QString& InLog, const QStringList& InWritten, const bool bInOK)
{
    Log->appendPlainText(InLog);
    CORE_LOG("[Import] " + InLog.toStdString())
    if(!bInOK) { ++Failures; }

    for(const QString& Path : InWritten) { StageAsset(Path); }
    Written << InWritten;

    Progress->setValue(Progress->maximum() - --JobsLeft);
    if(JobsLeft > 0)
    {
        return;
    }

    bRunning  = false;
    bFinished = true;
    Log->appendPlainText(Failures == 0 ? QStringLiteral("\nDone: %1 asset(s) written.").arg(Written.size())
                                       : QStringLiteral("\n%1 file(s) failed; %2 asset(s) written.").arg(Failures).arg(Written.size()));
    static_cast<ConditionPage*>(page(Page_Import))->Recheck();
    emit Imported(Written);
}

#endif // WITH_EDITOR
