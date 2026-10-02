#if WITH_EDITOR

#include "Private/Editor/Content/MeshViewerWindow.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include "Public/Core/CoreArchive.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Content/MeshPreviewView.h"

namespace
{
    const char* const DEFAULT_MATERIAL = "Engine/Content/Materials/DefaultLit.material.nmgraph";   // what an empty section draws with
    constexpr int     PREVIEW_SECTIONS = 8;              // Renderer::MeshPreview's slots

    QList<QPointer<MeshViewerWindow>>& OpenViewers()
    {
        static QList<QPointer<MeshViewerWindow>> Viewers;
        return Viewers;
    }

    /*
     * A section's material graph, by its runtime path: typed with completion
     * from the project's and the engine's, picked from the list, or dropped
     * from the Content Browser (a .material.nmgraph). Empty: DefaultLit.
     */
    class MaterialPicker final : public QComboBox
    {
    public:
        explicit MaterialPicker(const QStringList& InNames, QWidget* InParent = nullptr) : QComboBox(InParent)
        {
            setEditable(true);
            setInsertPolicy(QComboBox::NoInsert);
            addItem(QString());
            addItems(InNames);
            lineEdit()->setPlaceholderText(QString::fromLatin1(DEFAULT_MATERIAL));
            setAcceptDrops(true);
            lineEdit()->setAcceptDrops(false);   // the combo takes the drop, not its text
        }

    protected:
        void dragEnterEvent(QDragEnterEvent* InEvent) override
        {
            if(!EditorContent::MaterialPathFor(EditorContent::SingleDroppedFile(InEvent->mimeData())).isEmpty()) { InEvent->acceptProposedAction(); }
            else { InEvent->ignore(); }
        }

        void dropEvent(QDropEvent* InEvent) override
        {
            const QString Name = EditorContent::MaterialPathFor(EditorContent::SingleDroppedFile(InEvent->mimeData()));
            if(Name.isEmpty()) { InEvent->ignore(); return; }
            InEvent->acceptProposedAction();
            setCurrentText(Name);
            emit activated(currentIndex());
        }
    };
}

MeshViewerWindow* MeshViewerWindow::Open(const QString& InPath, QWidget* InParent)
{
    const QString Path = QFileInfo(InPath).absoluteFilePath();
    for(const QPointer<MeshViewerWindow>& Each : OpenViewers())
    {
        if(Each && Each->GetPath().compare(Path, Qt::CaseInsensitive) == 0)
        {
            Each->show();
            Each->raise();
            Each->activateWindow();
            return Each;
        }
    }

    auto* Viewer = new MeshViewerWindow(Path);
    QString Error;
    if(!Viewer->Load(Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("Open Mesh"), QStringLiteral("Could not open %1\n\n%2")
                             .arg(QFileInfo(Path).fileName(), Error));
        delete Viewer;
        return nullptr;
    }
    Viewer->BuildUi();
    OpenViewers().removeAll(nullptr);
    OpenViewers() << Viewer;
    Viewer->resize(1000, 620);
    Viewer->show();
    return Viewer;
}

MeshViewerWindow::MeshViewerWindow(const QString& InPath)
    : QMainWindow(nullptr)
    , Path(InPath)
    , RuntimePath(EditorContent::ToRuntimePath(InPath))
    , bReadOnly(EditorContent::IsEngineContent(InPath))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("%1 - Mesh").arg(QFileInfo(InPath).fileName()));
}

bool MeshViewerWindow::Load(QString& OutError)
{
    QFile File(Path);
    if(!File.open(QIODevice::ReadOnly))
    {
        OutError = File.errorString();
        return false;
    }
    const QByteArray Bytes = File.readAll();
    EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
    RenderCore::MeshData Loaded;
    if(!RenderCore::SerializeMesh(Ar, Loaded))
    {
        OutError = QStringLiteral("Not a mesh this editor can read (the log says why)");
        return false;
    }
    if(RuntimePath.isEmpty())
    {
        OutError = QStringLiteral("It is outside the project and the engine's content, so the renderer cannot load it");
        return false;
    }
    Mesh = std::move(Loaded);

    /* A mesh without sections is one section of everything. */
    Saved.clear();
    for(const RenderCore::MeshSection& Section : Mesh.Sections) { Saved << QString::fromStdString(Section.Material); }
    if(Saved.isEmpty()) { Saved << QString(); }
    Edited = Saved;
    return true;
}

void MeshViewerWindow::BuildUi()
{
    Preview = new MeshPreviewView(this);
    Preview->SetMesh(RuntimePath);

    auto* Side   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Side);

    Info = new QLabel(Side);
    Info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Info->setWordWrap(true);
    const auto Size = [&](const int InAxis) { return double(Mesh.BoundsMax[InAxis] - Mesh.BoundsMin[InAxis]); };
    Info->setText(QStringLiteral("<b>%1</b><br>%2 vertices, %3 triangles, %4 section(s)<br>Size %5 x %6 x %7%8%9")
                  .arg(RuntimePath.toHtmlEscaped()).arg(Mesh.GetVertexCount()).arg(Mesh.GetTriangleCount()).arg(Mesh.Sections.size())
                  .arg(Size(0), 0, 'g', 4).arg(Size(1), 0, 'g', 4).arg(Size(2), 0, 'g', 4)
                  .arg(Mesh.Colors.empty() ? QString() : QStringLiteral("<br>Vertex colours"))
                  .arg(Mesh.ImportInfo.IsEmpty() ? QString()
                       : QStringLiteral("<br>From %1").arg(QString::fromStdString(Mesh.ImportInfo.SourcePath).toHtmlEscaped())));
    Layout->addWidget(Info);

    auto* Heading = new QLabel(QStringLiteral("<b>Materials</b>"), Side);
    Layout->addWidget(Heading);
    auto* Note = new QLabel(bReadOnly
                            ? QStringLiteral("Engine content: a pick here only changes the preview.")
                            : QStringLiteral("Each section's own material: what a RenderComponent draws it with when its slot "
                                             "is empty. Empty is DefaultLit. Reimporting the mesh replaces them."), Side);
    Note->setWordWrap(true);
    Note->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(Note);

    Table = new QTableWidget(0, 2, Side);
    Table->setHorizontalHeaderLabels({ QStringLiteral("Section"), QStringLiteral("Material") });
    Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    Table->verticalHeader()->setVisible(false);
    Table->setSelectionMode(QAbstractItemView::NoSelection);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Layout->addWidget(Table, 1);

    auto* Animate = new QCheckBox(QStringLiteral("Animate"), Side);
    Animate->setToolTip(QStringLiteral("Keep drawing, for materials that move with Time"));
    connect(Animate, &QCheckBox::toggled, Preview, &MeshPreviewView::SetAnimate);
    auto* ResetView = new QPushButton(QStringLiteral("Reset View"), Side);
    connect(ResetView, &QPushButton::clicked, Preview, &MeshPreviewView::ResetView);
    auto* ViewRow = new QHBoxLayout();
    ViewRow->addWidget(Animate);
    ViewRow->addStretch(1);
    ViewRow->addWidget(ResetView);
    Layout->addLayout(ViewRow);

    SaveButton   = new QPushButton(QStringLiteral("Save"), Side);
    RevertButton = new QPushButton(QStringLiteral("Revert"), Side);
    SaveButton->setToolTip(bReadOnly ? QStringLiteral("The engine's meshes are read-only here")
                                     : QStringLiteral("Write these materials into the mesh (Ctrl+S)"));
    connect(SaveButton, &QPushButton::clicked, this, &MeshViewerWindow::Save);
    connect(RevertButton, &QPushButton::clicked, this, &MeshViewerWindow::Revert);
    auto* SaveRow = new QHBoxLayout();
    SaveRow->addStretch(1);
    SaveRow->addWidget(RevertButton);
    SaveRow->addWidget(SaveButton);
    Layout->addLayout(SaveRow);

    Status = new QLabel(Side);
    Status->setWordWrap(true);
    Status->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(Status);
    connect(Preview, &MeshPreviewView::Drawn, this, [this](const QString& InError) { Status->setText(InError); });

    auto* SaveKey = new QAction(this);
    SaveKey->setShortcut(QKeySequence::Save);
    connect(SaveKey, &QAction::triggered, this, &MeshViewerWindow::Save);
    addAction(SaveKey);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Preview);
    Split->addWidget(Side);
    Split->setStretchFactor(0, 1);
    Split->setStretchFactor(1, 0);
    Split->setSizes({ 640, 340 });
    setCentralWidget(Split);

    FillSections();
    UpdatePreview();
    UpdateState();
}

void MeshViewerWindow::FillSections()
{
    const QStringList Names = EditorContent::ListMaterialPaths();
    Table->setRowCount(Edited.size());
    for(int Row = 0; Row < Edited.size(); ++Row)
    {
        const QString Name = Row < int(Mesh.Sections.size()) && !Mesh.Sections[size_t(Row)].Name.empty()
                             ? QString::fromStdString(Mesh.Sections[size_t(Row)].Name)
                             : (Mesh.Sections.empty() ? QStringLiteral("(whole mesh)") : QStringLiteral("Section %1").arg(Row));
        auto* Label = new QTableWidgetItem(Name);
        Label->setToolTip(QStringLiteral("Section %1%2").arg(Row)
                          .arg(Row >= PREVIEW_SECTIONS ? QStringLiteral(" -- past the eighth, the preview shows its saved material") : QString()));
        Table->setItem(Row, 0, Label);

        auto* Picker = new MaterialPicker(Names, Table);
        Picker->setCurrentText(Edited[Row]);
        const auto Picked = [this, Picker, Row]()
        {
            const QString Text = Picker->currentText().trimmed();
            if(Text == Edited[Row]) { return; }
            if(!EditorContent::IsMaterialFieldValue(Text))
            {
                /* Material graphs only: what was typed is refused. */
                QToolTip::showText(Picker->mapToGlobal(QPoint(0, Picker->height())),
                                   QStringLiteral("Not a material graph: give a *.material.nmgraph path"), Picker);
                Picker->setCurrentText(Edited[Row]);
                return;
            }
            Edited[Row] = Text;
            UpdatePreview();
            UpdateState();
        };
        connect(Picker, QOverload<int>::of(&QComboBox::activated), this, Picked);
        connect(Picker->lineEdit(), &QLineEdit::editingFinished, this, Picked);
        Table->setCellWidget(Row, 1, Picker);
    }
}

QStringList MeshViewerWindow::PreviewSections() const
{
    QStringList Out;
    for(const QString& Each : Edited) { Out << (Each.isEmpty() ? QString::fromLatin1(DEFAULT_MATERIAL) : Each); }
    return Out;
}

void MeshViewerWindow::UpdatePreview()
{
    Preview->SetSections(PreviewSections());
}

void MeshViewerWindow::UpdateState()
{
    const bool bDirty = Edited != Saved;
    SaveButton->setEnabled(bDirty && !bReadOnly);
    RevertButton->setEnabled(bDirty);
    setWindowModified(bDirty && !bReadOnly);
    setWindowTitle(QStringLiteral("%1%2 - Mesh%3").arg(QFileInfo(Path).fileName(),
                                                       bDirty && !bReadOnly ? QStringLiteral("*") : QString(),
                                                       bReadOnly ? QStringLiteral(" (engine, read-only)") : QString()));
}

bool MeshViewerWindow::Save()
{
    if(bReadOnly || Edited == Saved) { return true; }

    RenderCore::MeshData Updated = Mesh;
    if(Updated.Sections.empty())
    {
        RenderCore::MeshSection Whole;
        Whole.IndexCount = uint32_t(Updated.Indices.size());
        Updated.Sections.push_back(Whole);
    }
    for(int Row = 0; Row < Edited.size() && Row < int(Updated.Sections.size()); ++Row)
    {
        Updated.Sections[size_t(Row)].Material = Edited[Row].toStdString();
    }
    if(!RenderCore::SaveMeshFile(QFileInfo(Path).absoluteFilePath().toStdString(), Updated))
    {
        QMessageBox::warning(this, QStringLiteral("Save Mesh"), QStringLiteral("Could not write %1 -- see the log").arg(Path));
        return false;
    }

    Mesh  = std::move(Updated);
    Saved = Edited;
    EditorContent::StageAsset(Path);   // the level (and this preview) load it afresh
    Status->setText(QStringLiteral("Saved"));
    UpdatePreview();
    UpdateState();
    return true;
}

void MeshViewerWindow::Revert()
{
    Edited = Saved;
    FillSections();
    UpdatePreview();
    UpdateState();
}

void MeshViewerWindow::closeEvent(QCloseEvent* InEvent)
{
    if(!bReadOnly && Edited != Saved)
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Mesh"), QStringLiteral("Save the material changes to %1?").arg(QFileInfo(Path).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if(Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !Save()))
        {
            InEvent->ignore();
            return;
        }
    }
    InEvent->accept();
}

#endif // WITH_EDITOR
