#if WITH_EDITOR

#include "Private/Editor/Content/MeshViewerWindow.h"

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
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
#include "Public/RenderCore/SkeletonAsset.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"
#include "Private/Editor/Content/AnimationAssets.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Content/MeshPreviewView.h"
#include "Private/Editor/Graph/GraphAssets.h"

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
            MeshViewerWindow* Found = Each;
            OpenViewers().removeAll(Each);
            OpenViewers() << Found;   // the front one now
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

MeshViewerWindow* MeshViewerWindow::FindFront()
{
    OpenViewers().removeAll(nullptr);
    return OpenViewers().isEmpty() ? nullptr : OpenViewers().back().data();
}

MeshViewerWindow* MeshViewerWindow::OpenAnimation(const QString& InPath, QWidget* InParent)
{
    const std::string Skeleton = EditorAnimation::SkeletonOfAnimation(InPath);
    const QString     MeshFile = Skeleton.empty() ? QString() : EditorAnimation::FindMeshOn(Skeleton);
    if(MeshFile.isEmpty())
    {
        QMessageBox::information(InParent, QStringLiteral("Preview Animation"),
                                 Skeleton.empty()
                                 ? QStringLiteral("%1 names no skeleton to play on.").arg(QFileInfo(InPath).fileName())
                                 : QStringLiteral("No skinned mesh on %1 is in the project: import the mesh it was made for, "
                                                  "with Import Animations on.").arg(QString::fromStdString(Skeleton)));
        return nullptr;
    }
    MeshViewerWindow* Viewer = Open(MeshFile, InParent);
    if(Viewer) { Viewer->SelectAnimation(EditorContent::ToRuntimePath(InPath)); }
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
    /* A skinned mesh is drawn posed, under a transport. */
    QWidget* Shown = nullptr;
    if(Mesh.IsSkinned())
    {
        Anim = new AnimPreviewWidget(this);
        Anim->SetMesh(RuntimePath, Mesh);
        Preview = Anim->GetView();
        Shown   = Anim;
    }
    else
    {
        Preview = new MeshPreviewView(this);
        Preview->SetMesh(RuntimePath);
        Shown = Preview;
    }

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

    /* Materials, and for a skinned mesh its animation and bones, as tabs. */
    QWidget* Panel = Side;
    if(Anim)
    {
        auto* Tabs = new QTabWidget(this);
        Tabs->addTab(Side, QStringLiteral("Materials"));
        Tabs->addTab(BuildAnimationTab(), QStringLiteral("Animation"));
        Tabs->addTab(BuildSkeletonTab(), QStringLiteral("Skeleton"));
        Tabs->setCurrentIndex(1);
        Panel = Tabs;
    }

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Shown);
    Split->addWidget(Panel);
    Split->setStretchFactor(0, 1);
    Split->setStretchFactor(1, 0);
    Split->setSizes({ 640, 340 });
    setCentralWidget(Split);

    FillSections();
    UpdatePreview();
    UpdateState();
}

// === Animation ======================================================================

QWidget* MeshViewerWindow::BuildAnimationTab()
{
    auto* Page   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Page);

    if(!Anim->HasSkeleton())
    {
        auto* Missing = new QLabel(QStringLiteral("Its skeleton, %1, will not load: it is drawn in its bind pose.")
                                   .arg(QString::fromStdString(Mesh.Skeleton).toHtmlEscaped()), Page);
        Missing->setWordWrap(true);
        Layout->addWidget(Missing);
    }

    /* Any clip made on this skeleton, or any anim state. */
    AnimationPicker = new QComboBox(Page);
    AnimationPicker->addItem(QStringLiteral("(Reference pose)"), QString());
    for(const QString& Each : EditorContent::ListAnimationPaths())
    {
        if(Each.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive))
        {
            const std::shared_ptr<const RenderCore::AnimClipData> Clip = RenderCore::AnimClipAssetCache::Find(Each.toStdString());
            if(!Clip || Clip->Skeleton != Mesh.Skeleton) { continue; }
        }
        AnimationPicker->addItem(Each, Each);
    }
    AnimationPicker->setToolTip(QStringLiteral("The clips imported with this skeleton, and every anim state (.nmanimstate)"));
    connect(AnimationPicker, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        SelectAnimation(AnimationPicker->itemData(InIndex).toString());
    });
    auto* PickRow = new QFormLayout();
    PickRow->addRow(QStringLiteral("Play"), AnimationPicker);
    Layout->addLayout(PickRow);

    StateLabel = new QLabel(Page);
    StateLabel->setWordWrap(true);
    StateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Layout->addWidget(StateLabel);

    NotifyFlash = new QLabel(Page);
    NotifyFlash->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(NotifyFlash);
    auto* FlashOff = new QTimer(this);
    FlashOff->setSingleShot(true);
    FlashOff->setInterval(700);
    connect(FlashOff, &QTimer::timeout, NotifyFlash, &QLabel::clear);
    connect(Anim, &AnimPreviewWidget::NotifyFired, this, [this, FlashOff](const QString& InName)
    {
        NotifyFlash->setText(QStringLiteral("Notify: %1").arg(InName));
        FlashOff->start();
    });

    /* An anim state's parameters, to drive it by hand. */
    ParameterBox  = new QGroupBox(QStringLiteral("Parameters"), Page);
    ParameterForm = new QFormLayout(ParameterBox);
    Layout->addWidget(ParameterBox);

    /* A clip's own settings, saved into it. */
    ClipBox = new QGroupBox(QStringLiteral("Clip"), Page);
    auto* ClipLayout = new QVBoxLayout(ClipBox);
    ClipLoop = new QCheckBox(QStringLiteral("Loops"), ClipBox);
    ClipLoop->setToolTip(QStringLiteral("Whether an Animation component (or an anim state set to the clip's own) loops it"));
    ClipInPlace = new QCheckBox(QStringLiteral("In place (hold the root bone's X and Z)"), ClipBox);
    ClipInPlace->setToolTip(QStringLiteral("Root motion: on, a walk walks on the spot; the entity's own movement carries it"));
    const auto ClipEdited = [this]()
    {
        ClipEdit.bLoop      = ClipLoop->isChecked();
        ClipEdit.RootMotion = ClipInPlace->isChecked() ? RenderCore::EAnimRootMotion::InPlace : RenderCore::EAnimRootMotion::Keep;
        bClipDirty = true;
        const float Time = Anim->GetInstance().GetTime();
        Anim->PlayClip(std::make_shared<RenderCore::AnimClipData>(ClipEdit));
        Anim->Seek(Time);
        UpdateAnimationState();
    };
    connect(ClipLoop, &QCheckBox::toggled, this, ClipEdited);
    connect(ClipInPlace, &QCheckBox::toggled, this, ClipEdited);
    ClipLayout->addWidget(ClipLoop);
    ClipLayout->addWidget(ClipInPlace);

    auto* NotifyHeading = new QLabel(QStringLiteral("Notifies -- each fires \"AnimNotify\" (entity, name) as the clip passes it"), ClipBox);
    NotifyHeading->setWordWrap(true);
    ClipLayout->addWidget(NotifyHeading);
    NotifyTable = new QTableWidget(0, 2, ClipBox);
    NotifyTable->setHorizontalHeaderLabels({ QStringLiteral("Name"), QStringLiteral("Time (s)") });
    NotifyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    NotifyTable->verticalHeader()->setVisible(false);
    connect(NotifyTable, &QTableWidget::cellChanged, this, [this](const int InRow, const int InColumn)
    {
        if(InRow < 0 || size_t(InRow) >= ClipEdit.Notifies.size()) { return; }
        const QString Text = NotifyTable->item(InRow, InColumn)->text().trimmed();
        if(InColumn == 0) { ClipEdit.Notifies[size_t(InRow)].Name = Text.toStdString(); }
        else
        {
            bool bOK = false;
            const float Time = Text.toFloat(&bOK);
            if(bOK) { ClipEdit.Notifies[size_t(InRow)].Time = std::clamp(Time, 0.0f, ClipEdit.GetDuration()); }
        }
        bClipDirty = true;
        Anim->PlayClip(std::make_shared<RenderCore::AnimClipData>(ClipEdit));
        UpdateAnimationState();
    });
    ClipLayout->addWidget(NotifyTable, 1);

    auto* AddNotify = new QPushButton(QStringLiteral("Add at Playhead"), ClipBox);
    connect(AddNotify, &QPushButton::clicked, this, [this]()
    {
        ClipEdit.Notifies.push_back({ "Notify", Anim->GetInstance().GetTime() });
        std::stable_sort(ClipEdit.Notifies.begin(), ClipEdit.Notifies.end(),
                         [](const RenderCore::AnimNotify& InA, const RenderCore::AnimNotify& InB) { return InA.Time < InB.Time; });
        bClipDirty = true;
        FillNotifies();
        UpdateAnimationState();
    });
    auto* RemoveNotify = new QPushButton(QStringLiteral("Remove"), ClipBox);
    connect(RemoveNotify, &QPushButton::clicked, this, [this]()
    {
        const int Row = NotifyTable->currentRow();
        if(Row < 0 || size_t(Row) >= ClipEdit.Notifies.size()) { return; }
        ClipEdit.Notifies.erase(ClipEdit.Notifies.begin() + Row);
        bClipDirty = true;
        FillNotifies();
        UpdateAnimationState();
    });
    SaveClipButton = new QPushButton(QStringLiteral("Save Clip"), ClipBox);
    connect(SaveClipButton, &QPushButton::clicked, this, &MeshViewerWindow::SaveClip);
    auto* NotifyRow = new QHBoxLayout();
    NotifyRow->addWidget(AddNotify);
    NotifyRow->addWidget(RemoveNotify);
    NotifyRow->addStretch(1);
    NotifyRow->addWidget(SaveClipButton);
    ClipLayout->addLayout(NotifyRow);
    Layout->addWidget(ClipBox, 1);
    Layout->addStretch(0);

    connect(Anim, &AnimPreviewWidget::Advanced, this, &MeshViewerWindow::UpdateAnimationState);
    SelectAnimation(QString());
    return Page;
}

QWidget* MeshViewerWindow::BuildSkeletonTab()
{
    auto* Tree = new QTreeWidget(this);
    Tree->setHeaderLabels({ QStringLiteral("Bone"), QStringLiteral("Index") });
    Tree->setToolTip(QStringLiteral("The bones, as sockets and layer masks name them"));
    if(const RenderCore::SkeletonData* Skeleton = Anim->GetSkeleton())
    {
        std::vector<QTreeWidgetItem*> Items(Skeleton->Bones.size(), nullptr);
        for(size_t Bone = 0; Bone < Skeleton->Bones.size(); ++Bone)
        {
            const int32_t Parent = Skeleton->Bones[Bone].Parent;
            const QStringList Columns = { QString::fromStdString(Skeleton->Bones[Bone].Name), QString::number(Bone) };
            Items[Bone] = Parent >= 0 ? new QTreeWidgetItem(Items[size_t(Parent)], Columns) : new QTreeWidgetItem(Tree, Columns);
        }
        Tree->expandAll();
        Tree->resizeColumnToContents(0);
    }
    return Tree;
}

void MeshViewerWindow::SelectAnimation(const QString& InRuntimePath)
{
    if(!Anim) { return; }
    if(bClipDirty && !ClipFile.isEmpty())
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Clip"), QStringLiteral("Save the changes to %1?").arg(QFileInfo(ClipFile).fileName()),
            QMessageBox::Save | QMessageBox::Discard, QMessageBox::Save);
        if(Answer == QMessageBox::Save) { SaveClip(); }
    }

    AnimationPath = InRuntimePath;
    ClipFile.clear();
    ClipEdit   = RenderCore::AnimClipData();
    bClipDirty = false;
    State.reset();

    if(InRuntimePath.endsWith(QString::fromLatin1(RenderCore::ANIM_STATE_EXTENSION), Qt::CaseInsensitive))
    {
        State = RenderCore::AnimStateAssetCache::Find(InRuntimePath.toStdString());
        Anim->PlayState(State);
    }
    else if(InRuntimePath.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive))
    {
        /* Edited as on disk; played as edited. */
        ClipFile = EditorAnimation::FileOfRuntimePath(InRuntimePath);
        EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(ClipFile);
        if(!RenderCore::SerializeAnimClip(Ar, ClipEdit))
        {
            ClipFile.clear();
            ClipEdit = RenderCore::AnimClipData();
        }
        Anim->PlayClip(ClipFile.isEmpty() ? RenderCore::AnimClipAssetCache::Find(InRuntimePath.toStdString())
                                          : std::make_shared<RenderCore::AnimClipData>(ClipEdit));
    }
    else
    {
        Anim->PlayClip(nullptr);
    }

    if(AnimationPicker)
    {
        int Index = AnimationPicker->findData(InRuntimePath);
        if(Index < 0 && !InRuntimePath.isEmpty())
        {
            AnimationPicker->addItem(InRuntimePath, InRuntimePath);
            Index = AnimationPicker->count() - 1;
        }
        AnimationPicker->setCurrentIndex(std::max(Index, 0));
    }
    FillParameters();
    FillNotifies();
    UpdateAnimationState();
}

void MeshViewerWindow::FillParameters()
{
    while(ParameterForm->rowCount() > 0) { ParameterForm->removeRow(0); }
    ParameterBox->setVisible(State && !State->Parameters.empty());
    if(!State) { return; }

    for(const RenderCore::AnimStateParameter& Parameter : State->Parameters)
    {
        const std::string Name  = Parameter.Name;
        const QString     Label = QString::fromStdString(Name);
        switch(Parameter.Type)
        {
            case RenderCore::EAnimParameterType::Float:
            {
                auto* Value = new QDoubleSpinBox(ParameterBox);
                Value->setRange(-100000.0, 100000.0);
                Value->setDecimals(2);
                Value->setValue(double(Parameter.Default));
                connect(Value, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, Name](const double InValue)
                {
                    Anim->GetInstance().SetParameter(Name, float(InValue));
                    if(!Anim->IsPlaying()) { Anim->Repose(); }
                });
                ParameterForm->addRow(Label, Value);
                break;
            }
            case RenderCore::EAnimParameterType::Bool:
            {
                auto* Value = new QCheckBox(ParameterBox);
                Value->setChecked(Parameter.Default != 0.0f);
                connect(Value, &QCheckBox::toggled, this, [this, Name](const bool bInValue)
                {
                    Anim->GetInstance().SetParameter(Name, bInValue ? 1.0f : 0.0f);
                    if(!Anim->IsPlaying()) { Anim->Repose(); }
                });
                ParameterForm->addRow(Label, Value);
                break;
            }
            case RenderCore::EAnimParameterType::Trigger:
            {
                auto* Fire = new QPushButton(QStringLiteral("Fire"), ParameterBox);
                connect(Fire, &QPushButton::clicked, this, [this, Name]()
                {
                    Anim->GetInstance().SetParameter(Name, 1.0f);
                    if(!Anim->IsPlaying()) { Anim->SetPlaying(true); }
                });
                ParameterForm->addRow(Label, Fire);
                break;
            }
        }
    }
}

void MeshViewerWindow::FillNotifies()
{
    ClipBox->setVisible(!ClipFile.isEmpty());
    if(ClipFile.isEmpty()) { return; }

    const QSignalBlocker Block(NotifyTable);
    {
        const QSignalBlocker BlockLoop(ClipLoop);
        const QSignalBlocker BlockPlace(ClipInPlace);
        ClipLoop->setChecked(ClipEdit.bLoop);
        ClipInPlace->setChecked(ClipEdit.RootMotion == RenderCore::EAnimRootMotion::InPlace);
        ClipInPlace->setToolTip(QStringLiteral("Root motion: on, the root bone (%1) keeps its first frame's X and Z -- a walk walks on the spot")
                                .arg(QString::fromStdString(ClipEdit.RootBone)));
    }
    NotifyTable->setRowCount(int(ClipEdit.Notifies.size()));
    for(size_t Row = 0; Row < ClipEdit.Notifies.size(); ++Row)
    {
        NotifyTable->setItem(int(Row), 0, new QTableWidgetItem(QString::fromStdString(ClipEdit.Notifies[Row].Name)));
        NotifyTable->setItem(int(Row), 1, new QTableWidgetItem(QString::number(double(ClipEdit.Notifies[Row].Time), 'f', 3)));
    }
    const bool bReadOnlyClip = EditorContent::IsEngineContent(ClipFile);
    NotifyTable->setEnabled(!bReadOnlyClip);
    ClipLoop->setEnabled(!bReadOnlyClip);
    ClipInPlace->setEnabled(!bReadOnlyClip);
}

void MeshViewerWindow::UpdateAnimationState()
{
    if(!StateLabel) { return; }
    RenderCore::AnimInstance& Instance = Anim->GetInstance();
    QString Text;
    if(State)
    {
        for(size_t Layer = 0; Layer < Instance.GetLayerCount() && Layer < State->Layers.size(); ++Layer)
        {
            Text += QStringLiteral("%1<b>%2</b>: %3%4").arg(Text.isEmpty() ? QString() : QStringLiteral("<br>"))
                    .arg(QString::fromStdString(State->Layers[Layer].Name).toHtmlEscaped())
                    .arg(QString::fromUtf8(Instance.GetCurrentStateName(Layer)).toHtmlEscaped())
                    .arg(Instance.IsInTransition(Layer) ? QStringLiteral(" (blending in)") : QString());
        }
    }
    else if(!ClipFile.isEmpty())
    {
        Text = QStringLiteral("%1 frames at %2 fps, %3 tracks%4").arg(ClipEdit.FrameCount).arg(double(ClipEdit.SampleRate))
               .arg(ClipEdit.Tracks.size()).arg(bClipDirty ? QStringLiteral(" -- <b>unsaved</b>") : QString());
    }
    else
    {
        Text = QStringLiteral("The reference pose: the skeleton as it binds, with nothing playing.");
    }
    StateLabel->setText(Text);
    SaveClipButton->setEnabled(bClipDirty && !EditorContent::IsEngineContent(ClipFile));
}

bool MeshViewerWindow::SaveClip()
{
    if(ClipFile.isEmpty() || !bClipDirty) { return true; }
    if(!RenderCore::SaveAnimClipFile(ClipFile.toStdString(), ClipEdit))
    {
        QMessageBox::warning(this, QStringLiteral("Save Clip"), QStringLiteral("Could not write %1 -- see the log").arg(ClipFile));
        return false;
    }
    bClipDirty = false;
    EditorContent::StageAsset(ClipFile);   // levels playing it load it afresh
    Status->setText(QStringLiteral("Saved %1").arg(QFileInfo(ClipFile).fileName()));
    UpdateAnimationState();
    return true;
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
    if(bClipDirty && !ClipFile.isEmpty())
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Clip"), QStringLiteral("Save the changes to %1?").arg(QFileInfo(ClipFile).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if(Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !SaveClip()))
        {
            InEvent->ignore();
            return;
        }
    }
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
