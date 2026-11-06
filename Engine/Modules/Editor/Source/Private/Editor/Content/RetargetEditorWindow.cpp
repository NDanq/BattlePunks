#if WITH_EDITOR

#include "Private/Editor/Content/RetargetEditorWindow.h"
#include "Private/Editor/EditorIcons.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimRetarget.h"
#include "Public/RenderCore/HumanoidRig.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"
#include "Private/Editor/Content/AnimationAssets.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace
{
    QList<QPointer<RetargetEditorWindow>>& OpenEditors()
    {
        static QList<QPointer<RetargetEditorWindow>> Editors;
        return Editors;
    }

    QString ToQ(const std::string& InText) { return QString::fromStdString(InText); }

    const QStringList TRANSLATION_NAMES = { QStringLiteral("Skeleton (its own)"), QStringLiteral("Animation"),
                                            QStringLiteral("Animation, scaled"), QStringLiteral("Relative, scaled") };
    const QStringList TRANSLATION_TIPS = {
        QStringLiteral("The target's own reference translation: its bone lengths kept (every bone but the pelvis, usually)"),
        QStringLiteral("The source's translation as animated, unscaled"),
        QStringLiteral("The source's translation, scaled by the root scale: the pelvis"),
        QStringLiteral("The target's reference plus how far the source moved from its own, scaled") };

    constexpr int COLUMN_TARGET = 0, COLUMN_SOURCE = 1, COLUMN_TRANSLATION = 2, COLUMN_OFFSET = 3;
    constexpr size_t UNDO_LIMIT = 200;

    /* "none", or the offset's angle. */
    QString DescribeOffset(const RenderCore::AnimQuat& InOffset)
    {
        const float Degrees = 2.0f * std::acos(std::min(std::fabs(InOffset.W), 1.0f)) * 180.0f / 3.14159265f;
        return Degrees < 0.05f ? QStringLiteral("none") : QStringLiteral("custom (%1 deg)").arg(double(Degrees), 0, 'f', 1);
    }

    /* A skeleton by runtime path, read off disk (not the staged copies). */
    std::shared_ptr<const RenderCore::SkeletonData> ReadSkeletonAt(const std::string& InRuntime)
    {
        if(InRuntime.empty()) { return nullptr; }
        EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(EditorAnimation::FileOfRuntimePath(ToQ(InRuntime)));
        auto Skeleton = std::make_shared<RenderCore::SkeletonData>();
        return Ar.GetSize() > 0 && RenderCore::SerializeSkeleton(Ar, *Skeleton) ? Skeleton : nullptr;
    }

    /* A skinned mesh file read whole; false if it is not one. */
    bool ReadSkinnedMesh(const QString& InFile, RenderCore::MeshData& OutMesh)
    {
        if(InFile.isEmpty()) { return false; }
        EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(InFile);
        return Ar.GetSize() > 0 && RenderCore::SerializeMesh(Ar, OutMesh) && OutMesh.IsSkinned();
    }
}

// === Opening ==============================================================================

RetargetEditorWindow* RetargetEditorWindow::Open(const QString& InPath, QWidget* InParent)
{
    const QString Path = QFileInfo(InPath).absoluteFilePath();
    for(const QPointer<RetargetEditorWindow>& Each : OpenEditors())
    {
        if(Each && Each->GetPath().compare(Path, Qt::CaseInsensitive) == 0)
        {
            RetargetEditorWindow* Found = Each;
            OpenEditors().removeAll(Each);
            OpenEditors() << Found;
            Found->show();
            Found->raise();
            Found->activateWindow();
            return Found;
        }
    }

    auto* Editor = new RetargetEditorWindow(Path);
    QString Error;
    if(!Editor->Load(Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("Open Retarget"), QStringLiteral("Could not open %1\n\n%2")
                             .arg(QFileInfo(Path).fileName(), Error));
        delete Editor;
        return nullptr;
    }
    Editor->BuildUi();
    OpenEditors().removeAll(nullptr);
    OpenEditors() << Editor;
    Editor->resize(1680, 900);
    Editor->show();
    return Editor;
}

QString RetargetEditorWindow::CreateIn(const QString& InFolder, const QString& InTargetSkeleton, QWidget* InParent, const bool bInOpen)
{
    QString Target = InTargetSkeleton;
    if(Target.isEmpty())
    {
        const QStringList Skeletons = EditorContent::ListSkeletonPaths();
        bool bOK = false;
        Target = QInputDialog::getItem(InParent, QStringLiteral("New Retarget"),
                                       QStringLiteral("The skeleton its clips will play on (the target):"), Skeletons, 0, true, &bOK);
        if(!bOK || Target.trimmed().isEmpty()) { return QString(); }
        Target = Target.trimmed();
    }
    QString TargetName = QFileInfo(Target).completeBaseName();
    if(TargetName.endsWith(QStringLiteral("_Skeleton"))) { TargetName.chop(9); }

    bool bOK = false;
    const QString Name = QInputDialog::getText(InParent, QStringLiteral("New Retarget"), QStringLiteral("Name:"), QLineEdit::Normal,
                                               QStringLiteral("To_%1").arg(TargetName), &bOK).trimmed();
    if(!bOK || Name.isEmpty()) { return QString(); }
    const QString Folder = InFolder.isEmpty() ? QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content")) : InFolder;
    QString File = QDir(Folder).filePath(Name);
    if(!File.endsWith(QString::fromLatin1(RenderCore::RETARGET_EXTENSION), Qt::CaseInsensitive)) { File += QString::fromLatin1(RenderCore::RETARGET_EXTENSION); }
    if(QFileInfo::exists(File))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Retarget"), QStringLiteral("%1 already exists.").arg(QFileInfo(File).fileName()));
        return QString();
    }
    RenderCore::RetargetData Data;
    Data.Target = Target.toStdString();
    QDir().mkpath(Folder);
    if(!RenderCore::SaveRetargetFile(File.toStdString(), Data))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Retarget"), QStringLiteral("Could not write %1 -- see the log").arg(File));
        return QString();
    }
    EditorContent::StageAsset(File);
    if(bInOpen) { Open(File, InParent); }
    return File;
}

RetargetEditorWindow* RetargetEditorWindow::FindFront()
{
    OpenEditors().removeAll(nullptr);
    return OpenEditors().isEmpty() ? nullptr : OpenEditors().back().data();
}

RetargetEditorWindow::RetargetEditorWindow(const QString& InPath)
    : QMainWindow(nullptr)
    , Path(InPath)
    , bReadOnly(EditorContent::IsEngineContent(InPath))
{
    setAttribute(Qt::WA_DeleteOnClose);
}

bool RetargetEditorWindow::Load(QString& OutError)
{
    EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(Path);
    RenderCore::RetargetData Loaded;
    if(Ar.GetSize() == 0 || !RenderCore::SerializeRetarget(Ar, Loaded))
    {
        OutError = QStringLiteral("Not a retarget this editor can read (the log says why)");
        return false;
    }
    Saved   = Loaded;
    Edit    = std::move(Loaded);
    Current = Edit.Sources.empty() ? -1 : 0;
    return true;
}

// === Building =============================================================================

void RetargetEditorWindow::BuildUi()
{
    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(BuildMap());
    Split->addWidget(BuildPreview());
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setSizes({ 760, 920 });
    setCentralWidget(Split);

    const auto Shortcut = [this](const QKeySequence& InKeys, void (RetargetEditorWindow::*InSlot)())
    {
        auto* Action = new QAction(this);
        Action->setShortcut(InKeys);
        connect(Action, &QAction::triggered, this, InSlot);
        addAction(Action);
    };
    auto* SaveKey = new QAction(this);
    SaveKey->setShortcut(QKeySequence::Save);
    connect(SaveKey, &QAction::triggered, this, &RetargetEditorWindow::Save);
    addAction(SaveKey);
    Shortcut(QKeySequence::Undo, &RetargetEditorWindow::Undo);
    Shortcut(QKeySequence::Redo, &RetargetEditorWindow::Redo);

    FillAll();
    RestartPreview();
    UpdateTitle();
}

QWidget* RetargetEditorWindow::BuildMap()
{
    auto* Side   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Side);

    /* The target: what the clips play on. */
    auto* TargetBox = new QGroupBox(QStringLiteral("Target -- the skeleton the clips play on (an Animation component's mesh)"), Side);
    auto* TargetRow = new QHBoxLayout(TargetBox);
    TargetField = new QLineEdit(TargetBox);
    TargetField->setPlaceholderText(QStringLiteral("Content/Characters/Hero/Hero_Skeleton.nmskel"));
    auto* TargetCompleter = new QCompleter(EditorContent::ListSkeletonPaths(), TargetField);
    TargetCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    TargetCompleter->setFilterMode(Qt::MatchContains);
    TargetField->setCompleter(TargetCompleter);
    TargetField->setReadOnly(bReadOnly);
    connect(TargetField, &QLineEdit::editingFinished, this, [this]()
    {
        const std::string Value = TargetField->text().trimmed().toStdString();
        if(bFilling || Value == Edit.Target || Value.empty()) { return; }
        Remember();
        Edit.Target = Value;
        Edited(true);
        RestartPreview();
    });
    TargetRow->addWidget(TargetField, 1);
    TargetRow->addWidget(EditorContent::MakeRevealButton([this]() { return ToQ(Edit.Target); }, TargetBox));
    Layout->addWidget(TargetBox);

    /* The sources: one per skeleton whose clips it plays. */
    auto* SourceBox = new QGroupBox(QStringLiteral("Sources -- skeletons whose clips play on it"), Side);
    auto* SourceLayout = new QVBoxLayout(SourceBox);
    SourceList = new QListWidget(SourceBox);
    SourceList->setMaximumHeight(96);
    connect(SourceList, &QListWidget::currentRowChanged, this, [this](const int InRow)
    {
        if(bFilling) { return; }
        SelectSource(InRow);
    });
    SourceLayout->addWidget(SourceList);
    auto* SourceButtons = new QHBoxLayout();
    auto* AddSourceButton    = new QPushButton(QStringLiteral("Add Source..."), SourceBox);
    auto* RemoveSourceButton = new QPushButton(QStringLiteral("Remove"), SourceBox);
    AddSourceButton->setEnabled(!bReadOnly);
    RemoveSourceButton->setEnabled(!bReadOnly);
    connect(AddSourceButton, &QPushButton::clicked, this, [this]()
    {
        QStringList Choices;
        for(const QString& Each : EditorContent::ListSkeletonPaths())
        {
            if(Each.toStdString() != Edit.Target && !Edit.FindSource(Each.toStdString())) { Choices << Each; }
        }
        bool bOK = false;
        const QString Chosen = QInputDialog::getItem(this, QStringLiteral("Add Source"), QStringLiteral("A skeleton whose clips play on the target:"),
                                                     Choices, 0, true, &bOK).trimmed();
        if(bOK && !Chosen.isEmpty() && AddSource(Chosen)) { AutoMap(); }
    });
    connect(RemoveSourceButton, &QPushButton::clicked, this, [this]()
    {
        if(Current < 0 || size_t(Current) >= Edit.Sources.size()) { return; }
        Remember();
        Edit.Sources.erase(Edit.Sources.begin() + Current);
        Current = std::min(Current, int(Edit.Sources.size()) - 1);
        Edited(true);
        RestartPreview();
    });
    SourceButtons->addWidget(AddSourceButton);
    SourceButtons->addWidget(RemoveSourceButton);
    SourceButtons->addStretch(1);
    SourceLayout->addLayout(SourceButtons);
    Layout->addWidget(SourceBox);

    /* The selected source's map. */
    auto* MapBox = new QGroupBox(QStringLiteral("Bones -- which source bone drives each target bone"), Side);
    auto* MapLayout = new QVBoxLayout(MapBox);
    auto* Tools = new QHBoxLayout();
    const auto Tool = [&](const QString& InText, const QString& InTip, auto&& InClicked)
    {
        auto* Button = new QPushButton(InText, MapBox);
        Button->setToolTip(InTip);
        Button->setEnabled(!bReadOnly);
        connect(Button, &QPushButton::clicked, this, InClicked);
        Tools->addWidget(Button);
    };
    Tool(QStringLiteral("Auto Map"), QStringLiteral("Map every bone it can: by humanoid role (pelvis, spine, arms, legs, fingers...), "
                                                    "then by name. Replaces the map."), [this]() { AutoMap(); });
    Tool(QStringLiteral("Align Arms"), QStringLiteral("Offset the clavicles, arms and hands so the source's reference arms lie along "
                                                      "the target's (a T pose onto an A pose)"), [this]() { AlignArms(); });
    Tool(QStringLiteral("Reset Offsets"), QStringLiteral("Clear every pose offset"), [this]()
    {
        RenderCore::RetargetSource* Source = CurrentSource();
        if(!Source) { return; }
        Remember();
        for(RenderCore::RetargetBoneMap& Bone : Source->Bones) { Bone.PoseOffset = RenderCore::AnimQuat(); }
        Edited(true);
    });
    Tool(QStringLiteral("Clear"), QStringLiteral("Unmap every bone"), [this]() { ClearMap(); });
    Tools->addStretch(1);
    MapLayout->addLayout(Tools);

    auto* ScaleRow = new QHBoxLayout();
    RootScale = new QDoubleSpinBox(MapBox);
    RootScale->setRange(0.0, 100.0);
    RootScale->setDecimals(3);
    RootScale->setSingleStep(0.05);
    RootScale->setSpecialValueText(QStringLiteral("Automatic"));
    RootScale->setToolTip(QStringLiteral("What scaled translations (the pelvis's) are multiplied by. Automatic: the target pelvis's "
                                         "height over the source's"));
    RootScale->setEnabled(!bReadOnly);
    connect(RootScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        RenderCore::RetargetSource* Source = CurrentSource();
        if(bFilling || !Source) { return; }
        Remember();
        Source->RootScale = float(InValue);
        Edited(false);
    });
    RootScaleAuto = new QLabel(MapBox);
    ScaleRow->addWidget(new QLabel(QStringLiteral("Root Scale"), MapBox));
    ScaleRow->addWidget(RootScale);
    ScaleRow->addWidget(RootScaleAuto, 1);
    MapLayout->addLayout(ScaleRow);

    BoneTable = new QTableWidget(0, 4, MapBox);
    BoneTable->setHorizontalHeaderLabels({ QStringLiteral("Target Bone"), QStringLiteral("Source Bone"), QStringLiteral("Translation"),
                                           QStringLiteral("Pose Offset") });
    BoneTable->horizontalHeader()->setSectionResizeMode(COLUMN_TARGET, QHeaderView::Stretch);
    BoneTable->horizontalHeader()->setSectionResizeMode(COLUMN_SOURCE, QHeaderView::Stretch);
    BoneTable->verticalHeader()->setVisible(false);
    BoneTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    BoneTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    MapLayout->addWidget(BoneTable, 1);
    MapSummary = new QLabel(MapBox);
    MapSummary->setWordWrap(true);
    MapLayout->addWidget(MapSummary);
    Layout->addWidget(MapBox, 1);

    auto* SaveRow = new QHBoxLayout();
    auto* Bake = new QPushButton(QStringLiteral("Bake Clips..."), Side);
    Bake->setToolTip(QStringLiteral("Write chosen clips of the selected source as ordinary clips on the target: no runtime cost"));
    connect(Bake, &QPushButton::clicked, this, &RetargetEditorWindow::ShowBakeDialog);
    RevertButton = new QPushButton(QStringLiteral("Revert"), Side);
    connect(RevertButton, &QPushButton::clicked, this, [this]()
    {
        Remember();
        Edit = Saved;
        Current = Edit.Sources.empty() ? -1 : std::clamp(Current, 0, int(Edit.Sources.size()) - 1);
        Edited(true);
        bDirty = false;
        UpdateTitle();
        RestartPreview();
    });
    SaveButton = new QPushButton(QStringLiteral("Save"), Side);
    SaveButton->setToolTip(bReadOnly ? QStringLiteral("The engine's content is read-only here") : QStringLiteral("Write the file and restage it (Ctrl+S)"));
    connect(SaveButton, &QPushButton::clicked, this, &RetargetEditorWindow::Save);
    SaveRow->addWidget(Bake);
    SaveRow->addStretch(1);
    SaveRow->addWidget(RevertButton);
    SaveRow->addWidget(SaveButton);
    Layout->addLayout(SaveRow);
    return Side;
}

QWidget* RetargetEditorWindow::BuildPreview()
{
    auto* Side   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Side);
    auto* PickRow = new QFormLayout();
    ClipPicker = new QComboBox(Side);
    ClipPicker->setToolTip(QStringLiteral("A clip made on the selected source's skeleton, played on both"));
    connect(ClipPicker, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        PreviewClip(ClipPicker->itemData(InIndex).toString());
    });
    PickRow->addRow(QStringLiteral("Clip"), ClipPicker);
    Layout->addLayout(PickRow);

    auto* Pair = new QSplitter(Qt::Horizontal, Side);
    const auto Column = [&](QLabel*& OutLabel, AnimPreviewWidget*& OutPreview, const QString& InTitle)
    {
        auto* Box = new QGroupBox(InTitle, Pair);
        auto* BoxLayout = new QVBoxLayout(Box);
        OutLabel = new QLabel(Box);
        OutLabel->setWordWrap(true);
        BoxLayout->addWidget(OutLabel);
        OutPreview = new AnimPreviewWidget(Box);
        OutPreview->SetBonesShown(true);
        BoxLayout->addWidget(OutPreview, 1);
        Pair->addWidget(Box);
    };
    Column(SourceLabel, SourcePreview, QStringLiteral("Source -- as made"));
    Column(TargetLabel, TargetPreview, QStringLiteral("Target -- through the retarget (red: driven by no source bone)"));
    Layout->addWidget(Pair, 1);

    /* In step: the target follows the source's playhead. */
    connect(SourcePreview, &AnimPreviewWidget::Advanced, this, [this]()
    {
        if(SourcePreview->HasSkeleton() && TargetPreview->HasSkeleton()) { TargetPreview->Seek(SourcePreview->GetInstance().GetTime()); }
    });
    return Side;
}

// === Editing ==============================================================================

RenderCore::RetargetSource* RetargetEditorWindow::CurrentSource()
{
    return Current >= 0 && size_t(Current) < Edit.Sources.size() ? &Edit.Sources[size_t(Current)] : nullptr;
}

RenderCore::RetargetBoneMap& RetargetEditorWindow::MapFor(const std::string& InTarget)
{
    RenderCore::RetargetSource& Source = *CurrentSource();
    for(RenderCore::RetargetBoneMap& Bone : Source.Bones)
    {
        if(Bone.Target == InTarget) { return Bone; }
    }
    RenderCore::RetargetBoneMap Made;
    Made.Target = InTarget;
    Source.Bones.push_back(Made);
    return Source.Bones.back();
}

void RetargetEditorWindow::Remember()
{
    UndoStack.push_back(Edit);
    if(UndoStack.size() > UNDO_LIMIT) { UndoStack.erase(UndoStack.begin()); }
    RedoStack.clear();
}

void RetargetEditorWindow::Undo()
{
    if(UndoStack.empty() || bReadOnly) { return; }
    RedoStack.push_back(Edit);
    Edit = UndoStack.back();
    UndoStack.pop_back();
    Current = Edit.Sources.empty() ? -1 : std::clamp(Current, 0, int(Edit.Sources.size()) - 1);
    Edited(true);
    RestartPreview();
}

void RetargetEditorWindow::Redo()
{
    if(RedoStack.empty() || bReadOnly) { return; }
    UndoStack.push_back(Edit);
    Edit = RedoStack.back();
    RedoStack.pop_back();
    Current = Edit.Sources.empty() ? -1 : std::clamp(Current, 0, int(Edit.Sources.size()) - 1);
    Edited(true);
    RestartPreview();
}

void RetargetEditorWindow::Edited(const bool bInStructure)
{
    bDirty = true;
    if(bInStructure) { FillAll(); }
    else             { UpdateMarks(); }
    UpdateTitle();

    /* The target replays through the retarget as it is now. */
    if(TargetPreview && TargetPreview->HasSkeleton() && !ClipRuntime.isEmpty())
    {
        RenderCore::RetargetBindingCache::Clear();
        TargetPreview->GetInstance().SetRetarget(std::make_shared<RenderCore::RetargetData>(Edit));
        TargetPreview->PlayClip(RenderCore::AnimClipAssetCache::Find(ClipRuntime.toStdString()));
        TargetPreview->SetPlaying(false);
        TargetPreview->Seek(SourcePreview->HasSkeleton() ? SourcePreview->GetInstance().GetTime() : 0.0f);
    }
}

bool RetargetEditorWindow::AddSource(const QString& InSkeleton)
{
    const std::string Skeleton = InSkeleton.trimmed().toStdString();
    if(bReadOnly || Skeleton.empty() || Skeleton == Edit.Target || Edit.FindSource(Skeleton)) { return false; }
    Remember();
    RenderCore::RetargetSource Source;
    Source.Skeleton = Skeleton;
    Edit.Sources.push_back(Source);
    Current = int(Edit.Sources.size()) - 1;
    Edited(true);
    RestartPreview();
    return true;
}

void RetargetEditorWindow::SelectSource(const int InIndex)
{
    Current = InIndex >= 0 && size_t(InIndex) < Edit.Sources.size() ? InIndex : -1;
    LoadSkeletons();
    FillAll();
    RestartPreview();
}

size_t RetargetEditorWindow::AutoMap()
{
    RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source || bReadOnly || !SourceSkeleton || !TargetSkeleton) { return 0; }
    Remember();
    const float Scale = Source->RootScale;
    const size_t Mapped = RenderCore::AutoMapRetarget(*SourceSkeleton, *TargetSkeleton, *Source);
    Source->RootScale = Scale;
    Edited(true);
    return Mapped;
}

bool RetargetEditorWindow::AlignArms()
{
    RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source || bReadOnly || !SourceSkeleton || !TargetSkeleton) { return false; }
    Remember();
    const bool bAligned = RenderCore::AlignRetargetArms(*SourceSkeleton, *TargetSkeleton, *Source);
    if(!bAligned)
    {
        UndoStack.pop_back();
        QMessageBox::information(this, QStringLiteral("Align Arms"), QStringLiteral("The target is not a humanoid skeleton this can find the arms of."));
        return false;
    }
    Edited(true);
    return true;
}

void RetargetEditorWindow::ClearMap()
{
    RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source || bReadOnly) { return; }
    Remember();
    Source->Bones.clear();
    Edited(true);
}

// === Filling ==============================================================================

void RetargetEditorWindow::LoadSkeletons()
{
    TargetSkeleton = ReadSkeletonAt(Edit.Target);
    const RenderCore::RetargetSource* Source = CurrentSource();
    SourceSkeleton = Source ? ReadSkeletonAt(Source->Skeleton) : nullptr;
}

void RetargetEditorWindow::FillAll()
{
    LoadSkeletons();
    const QSignalBlocker Block(TargetField);
    bFilling = true;
    TargetField->setText(ToQ(Edit.Target));
    bFilling = false;
    FillSources();
    FillBones();
    FillClips();
}

void RetargetEditorWindow::FillSources()
{
    bFilling = true;
    SourceList->clear();
    for(const RenderCore::RetargetSource& Source : Edit.Sources)
    {
        SourceList->addItem(QStringLiteral("%1   (%2 bones mapped)").arg(ToQ(Source.Skeleton)).arg(Source.Bones.size()));
    }
    SourceList->setCurrentRow(Current);
    const RenderCore::RetargetSource* Source = CurrentSource();
    RootScale->setValue(Source ? double(Source->RootScale) : 0.0);
    RootScale->setEnabled(Source && !bReadOnly);
    bFilling = false;
}

void RetargetEditorWindow::FillBones()
{
    bFilling = true;
    BoneTable->setRowCount(0);
    const RenderCore::RetargetSource* Source = CurrentSource();
    if(!TargetSkeleton || !Source)
    {
        MapSummary->setText(!TargetSkeleton ? QStringLiteral("The target skeleton %1 cannot be read.").arg(ToQ(Edit.Target).toHtmlEscaped())
                                            : QStringLiteral("Add a source: a skeleton whose clips should play on the target."));
        bFilling = false;
        return;
    }
    QStringList SourceBones;
    SourceBones << QStringLiteral("(none)");
    if(SourceSkeleton)
    {
        for(const RenderCore::SkeletonBone& Bone : SourceSkeleton->Bones) { SourceBones << ToQ(Bone.Name); }
    }

    std::vector<int> Depth(TargetSkeleton->Bones.size(), 0);
    BoneTable->setRowCount(int(TargetSkeleton->Bones.size()));
    for(size_t Row = 0; Row < TargetSkeleton->Bones.size(); ++Row)
    {
        const RenderCore::SkeletonBone& Bone = TargetSkeleton->Bones[Row];
        Depth[Row] = Bone.Parent >= 0 ? Depth[size_t(Bone.Parent)] + 1 : 0;
        auto* Name = new QTableWidgetItem(QString(Depth[Row] * 2, QLatin1Char(' ')) + ToQ(Bone.Name));
        Name->setData(Qt::UserRole, ToQ(Bone.Name));
        BoneTable->setItem(int(Row), COLUMN_TARGET, Name);

        const RenderCore::RetargetBoneMap* Map = Source->FindBone(Bone.Name);
        const std::string Target = Bone.Name;

        auto* SourceCombo = new QComboBox(BoneTable);
        SourceCombo->addItems(SourceBones);
        if(Map && !Map->Source.empty())
        {
            const int Index = SourceCombo->findText(ToQ(Map->Source));
            if(Index < 0) { SourceCombo->addItem(ToQ(Map->Source)); }   // a bone the source lacks: kept, marked
            SourceCombo->setCurrentIndex(Index < 0 ? SourceCombo->count() - 1 : Index);
        }
        SourceCombo->setEnabled(!bReadOnly);
        connect(SourceCombo, QOverload<int>::of(&QComboBox::activated), this, [this, Target, SourceCombo](const int InIndex)
        {
            RenderCore::RetargetSource* Editing = CurrentSource();
            if(bFilling || !Editing) { return; }
            Remember();
            if(InIndex <= 0)
            {
                Editing->Bones.erase(std::remove_if(Editing->Bones.begin(), Editing->Bones.end(),
                                                    [&](const RenderCore::RetargetBoneMap& InBone) { return InBone.Target == Target; }),
                                     Editing->Bones.end());
            }
            else
            {
                MapFor(Target).Source = SourceCombo->itemText(InIndex).toStdString();
            }
            Edited(false);
        });
        BoneTable->setCellWidget(int(Row), COLUMN_SOURCE, SourceCombo);

        auto* ModeCombo = new QComboBox(BoneTable);
        for(int Mode = 0; Mode < TRANSLATION_NAMES.size(); ++Mode)
        {
            ModeCombo->addItem(TRANSLATION_NAMES[Mode]);
            ModeCombo->setItemData(Mode, TRANSLATION_TIPS[Mode], Qt::ToolTipRole);
        }
        ModeCombo->setCurrentIndex(Map ? int(Map->Translation) : 0);
        ModeCombo->setEnabled(!bReadOnly && Map != nullptr);
        connect(ModeCombo, QOverload<int>::of(&QComboBox::activated), this, [this, Target](const int InIndex)
        {
            if(bFilling || !CurrentSource()) { return; }
            Remember();
            MapFor(Target).Translation = RenderCore::ERetargetTranslation(InIndex);
            Edited(false);
        });
        BoneTable->setCellWidget(int(Row), COLUMN_TRANSLATION, ModeCombo);

        BoneTable->setItem(int(Row), COLUMN_OFFSET, new QTableWidgetItem(Map ? DescribeOffset(Map->PoseOffset) : QStringLiteral("-")));
    }
    BoneTable->resizeColumnToContents(COLUMN_TRANSLATION);
    BoneTable->resizeColumnToContents(COLUMN_OFFSET);
    bFilling = false;
    UpdateMarks();
}

void RetargetEditorWindow::FillClips()
{
    ClipPicker->clear();
    const RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source) { return; }
    for(const QString& Each : EditorContent::ListAnimationPaths())
    {
        if(!Each.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive)) { continue; }
        const std::shared_ptr<const RenderCore::AnimClipData> Clip = RenderCore::AnimClipAssetCache::Find(Each.toStdString());
        if(Clip && Clip->Skeleton == Source->Skeleton) { ClipPicker->addItem(Each, Each); }
    }
    const int Index = ClipPicker->findData(ClipRuntime);
    if(Index >= 0) { ClipPicker->setCurrentIndex(Index); }
}

void RetargetEditorWindow::UpdateMarks()
{
    const RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source || !TargetSkeleton) { return; }

    /* Rows: unmapped grey, a source bone the source lacks (or used twice) red. */
    std::vector<uint8_t> Unmapped(TargetSkeleton->Bones.size(), 1);
    size_t Mapped = 0, Bad = 0;
    std::map<std::string, int> Uses;
    for(const RenderCore::RetargetBoneMap& Bone : Source->Bones) { ++Uses[Bone.Source]; }
    for(size_t Row = 0; Row < TargetSkeleton->Bones.size() && int(Row) < BoneTable->rowCount(); ++Row)
    {
        const RenderCore::RetargetBoneMap* Map = Source->FindBone(TargetSkeleton->Bones[Row].Name);
        const bool bMapped = Map && !Map->Source.empty();
        const bool bBad = bMapped && ((SourceSkeleton && SourceSkeleton->FindBone(Map->Source) < 0) || Uses[Map->Source] > 1);
        Unmapped[Row] = bMapped && !bBad ? 0 : 1;
        Mapped += bMapped ? 1 : 0;
        Bad    += bBad ? 1 : 0;
        if(QTableWidgetItem* Name = BoneTable->item(int(Row), COLUMN_TARGET))
        {
            Name->setForeground(bBad ? QBrush(QColor(230, 80, 80)) : bMapped ? QBrush() : QBrush(QColor(140, 140, 140)));
            Name->setToolTip(bBad ? QStringLiteral("Its source bone is not in the source skeleton, or drives another bone too")
                                  : bMapped ? QString() : QStringLiteral("Unmapped: it keeps its reference pose under its parent"));
        }
        if(QTableWidgetItem* Offset = BoneTable->item(int(Row), COLUMN_OFFSET))
        {
            Offset->setText(Map ? DescribeOffset(Map->PoseOffset) : QStringLiteral("-"));
        }
        if(auto* Mode = qobject_cast<QComboBox*>(BoneTable->cellWidget(int(Row), COLUMN_TRANSLATION)))
        {
            const QSignalBlocker Block(Mode);
            Mode->setEnabled(!bReadOnly && bMapped);
            Mode->setCurrentIndex(Map ? int(Map->Translation) : 0);
        }
    }
    MapSummary->setText(QStringLiteral("%1 of %2 bones mapped%3. A bone left unmapped keeps its reference pose and follows its parent "
                                       "-- right for twist and helper bones.")
                        .arg(Mapped).arg(TargetSkeleton->Bones.size())
                        .arg(Bad ? QStringLiteral(", <b>%1 with a bad source</b>").arg(Bad) : QString()));
    if(!SourceSkeleton)
    {
        MapSummary->setText(MapSummary->text() + QStringLiteral(" The source skeleton %1 cannot be read.").arg(ToQ(Source->Skeleton).toHtmlEscaped()));
    }

    const float Auto = SourceSkeleton ? RenderCore::ComputeRetargetRootScale(*SourceSkeleton, *TargetSkeleton, *Source) : 1.0f;
    RootScaleAuto->setText(QStringLiteral("automatic: %1").arg(double(Auto), 0, 'f', 3));
    TargetPreview->SetMarkedBones(Unmapped);

    /* The source's bones that drive nothing, likewise. */
    if(SourceSkeleton)
    {
        std::vector<uint8_t> Idle(SourceSkeleton->Bones.size(), 1);
        for(const RenderCore::RetargetBoneMap& Bone : Source->Bones)
        {
            const int32_t Index = SourceSkeleton->FindBone(Bone.Source);
            if(Index >= 0) { Idle[size_t(Index)] = 0; }
        }
        SourcePreview->SetMarkedBones(Idle);
    }
}

void RetargetEditorWindow::UpdateTitle()
{
    const bool bShowDirty = bDirty && !bReadOnly;
    SaveButton->setEnabled(bShowDirty && !Edit.FindDefect());
    RevertButton->setEnabled(bShowDirty);
    setWindowTitle(QStringLiteral("%1%2 - Retarget%3").arg(QFileInfo(Path).fileName(), bShowDirty ? QStringLiteral("*") : QString(),
                                                           bReadOnly ? QStringLiteral(" (engine, read-only)") : QString()));
}

// === Preview ==============================================================================

void RetargetEditorWindow::RestartPreview()
{
    LoadSkeletons();
    const RenderCore::RetargetSource* Source = CurrentSource();

    /* A mesh on each skeleton, if the project or the engine has one. */
    const auto Show = [](const std::string& InSkeleton, QLabel* InLabel, AnimPreviewWidget* InPreview, QString& OutRuntime,
                         RenderCore::MeshData& OutMesh)
    {
        const QString File = EditorAnimation::FindMeshOn(InSkeleton);
        OutRuntime.clear();
        OutMesh = RenderCore::MeshData();
        if(ReadSkinnedMesh(File, OutMesh))
        {
            OutRuntime = EditorContent::ToRuntimePath(File);
            InPreview->SetMesh(OutRuntime, OutMesh);
            InLabel->setText(QStringLiteral("On <b>%1</b>").arg(OutRuntime.toHtmlEscaped()));
            return;
        }
        InLabel->setText(InSkeleton.empty() ? QStringLiteral("No skeleton.")
                         : QStringLiteral("No skinned mesh on %1 was found: nothing to show it on.").arg(ToQ(InSkeleton).toHtmlEscaped()));
    };
    Show(Source ? Source->Skeleton : std::string(), SourceLabel, SourcePreview, SourceMeshRuntime, SourceMesh);
    Show(Edit.Target, TargetLabel, TargetPreview, TargetMeshRuntime, TargetMesh);

    if(ClipPicker->findData(ClipRuntime) < 0)
    {
        ClipRuntime = ClipPicker->count() > 0 ? ClipPicker->itemData(0).toString() : QString();
    }
    PreviewClip(ClipRuntime);
}

bool RetargetEditorWindow::PreviewClip(const QString& InClipRuntime)
{
    ClipRuntime = InClipRuntime;
    const int Index = ClipPicker->findData(ClipRuntime);
    if(Index >= 0) { ClipPicker->setCurrentIndex(Index); }
    const std::shared_ptr<const RenderCore::AnimClipData> Clip =
        ClipRuntime.isEmpty() ? nullptr : RenderCore::AnimClipAssetCache::Find(ClipRuntime.toStdString());

    if(SourcePreview->HasSkeleton()) { SourcePreview->PlayClip(Clip); }
    if(TargetPreview->HasSkeleton())
    {
        RenderCore::RetargetBindingCache::Clear();
        TargetPreview->GetInstance().SetRetarget(std::make_shared<RenderCore::RetargetData>(Edit));
        TargetPreview->PlayClip(Clip);
        /* With a source to follow, the target is driven by its playhead. */
        if(SourcePreview->HasSkeleton()) { TargetPreview->SetPlaying(false); }
    }
    UpdateMarks();
    return Clip != nullptr;
}

// === Saving and baking ====================================================================

bool RetargetEditorWindow::Save()
{
    if(bReadOnly) { return true; }
    if(const char* Defect = Edit.FindDefect())
    {
        QMessageBox::warning(this, QStringLiteral("Save Retarget"), QStringLiteral("It cannot be saved as it is: %1").arg(QString::fromLatin1(Defect)));
        return false;
    }
    /* Unmapped entries (a source bone set back to none) are not written. */
    for(RenderCore::RetargetSource& Source : Edit.Sources)
    {
        Source.Bones.erase(std::remove_if(Source.Bones.begin(), Source.Bones.end(),
                                          [](const RenderCore::RetargetBoneMap& InBone) { return InBone.Source.empty(); }),
                           Source.Bones.end());
    }
    if(!RenderCore::SaveRetargetFile(Path.toStdString(), Edit))
    {
        QMessageBox::warning(this, QStringLiteral("Save Retarget"), QStringLiteral("Could not write %1 -- see the log").arg(Path));
        return false;
    }
    Saved  = Edit;
    bDirty = false;
    EditorContent::StageAsset(Path);   // characters playing through it rebind
    UpdateTitle();
    return true;
}

QStringList RetargetEditorWindow::BakeClips(const QStringList& InClipFiles, const QString& InFolder, const QString& InSuffix,
                                            QStringList& OutErrors)
{
    QStringList Written;
    if(bDirty && !bReadOnly && !Save())
    {
        OutErrors << QStringLiteral("the retarget must be saved before baking");
        return Written;
    }
    QDir().mkpath(InFolder);
    for(const QString& File : InClipFiles)
    {
        const QString Out = QDir(InFolder).filePath(QFileInfo(File).completeBaseName() + InSuffix + QStringLiteral(".nmanim"));
        const EditorContent::ImportResult Result = EditorContent::BakeRetargetClip(File, Path, Out, EditorGraphs::GetProjectRoot());
        if(!Result.bOK)
        {
            OutErrors << Result.Message;
            continue;
        }
        EditorContent::StageAsset(Out);
        Written << Out;
    }
    return Written;
}

void RetargetEditorWindow::ShowBakeDialog()
{
    const RenderCore::RetargetSource* Source = CurrentSource();
    if(!Source || ClipPicker->count() == 0)
    {
        QMessageBox::information(this, QStringLiteral("Bake Clips"), QStringLiteral("The selected source has no clips in the project to bake."));
        return;
    }
    QDialog Dialog(this);
    Dialog.setWindowTitle(QStringLiteral("Bake Clips"));
    auto* Layout = new QVBoxLayout(&Dialog);
    Layout->addWidget(new QLabel(QStringLiteral("Each clip chosen is written as an ordinary clip on %1, retargeted once and for all "
                                                "(reimporting it bakes it again).").arg(ToQ(Edit.Target).toHtmlEscaped()), &Dialog));
    auto* List = new QListWidget(&Dialog);
    for(int Index = 0; Index < ClipPicker->count(); ++Index)
    {
        auto* Item = new QListWidgetItem(ClipPicker->itemText(Index), List);
        Item->setData(Qt::UserRole, ClipPicker->itemData(Index));
        Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
        Item->setCheckState(Qt::Checked);
    }
    Layout->addWidget(List, 1);
    auto* Form = new QFormLayout();
    QString TargetName = QFileInfo(ToQ(Edit.Target)).completeBaseName();
    if(TargetName.endsWith(QStringLiteral("_Skeleton"))) { TargetName.chop(9); }
    auto* Folder = new QLineEdit(QFileInfo(Path).absolutePath(), &Dialog);
    auto* Browse = new QToolButton(&Dialog);
    Browse->setIcon(EditorIcons::Get(EditorIcons::EIcon::Browse));
    Browse->setToolTip(QStringLiteral("Choose the folder to bake into"));
    connect(Browse, &QToolButton::clicked, &Dialog, [&Dialog, Folder]()
    {
        const QString Chosen = QFileDialog::getExistingDirectory(&Dialog, QStringLiteral("Bake Into"), Folder->text());
        if(!Chosen.isEmpty()) { Folder->setText(Chosen); }
    });
    auto* FolderRow = new QHBoxLayout();
    FolderRow->addWidget(Folder, 1);
    FolderRow->addWidget(Browse);
    auto* Suffix = new QLineEdit(QStringLiteral("_") + TargetName, &Dialog);
    Form->addRow(QStringLiteral("Folder"), FolderRow);
    Form->addRow(QStringLiteral("Name suffix"), Suffix);
    Layout->addLayout(Form);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
    Buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Bake"));
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
    connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    Layout->addWidget(Buttons);
    Dialog.resize(640, 480);
    if(Dialog.exec() != QDialog::Accepted) { return; }

    QStringList Files;
    for(int Index = 0; Index < List->count(); ++Index)
    {
        if(List->item(Index)->checkState() == Qt::Checked)
        {
            Files << EditorContent::FileForRuntimePath(List->item(Index)->data(Qt::UserRole).toString());
        }
    }
    QStringList Errors;
    const QStringList Written = BakeClips(Files, Folder->text().trimmed(), Suffix->text().trimmed(), Errors);
    QMessageBox::information(this, QStringLiteral("Bake Clips"),
                             QStringLiteral("%1 clip(s) baked into %2.%3").arg(Written.size()).arg(QDir::toNativeSeparators(Folder->text().trimmed()))
                             .arg(Errors.isEmpty() ? QString() : QStringLiteral("\n\n") + Errors.join(QLatin1Char('\n'))));
}

void RetargetEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    if(bDirty && !bReadOnly)
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Retarget"), QStringLiteral("Save the changes to %1?").arg(QFileInfo(Path).fileName()),
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
