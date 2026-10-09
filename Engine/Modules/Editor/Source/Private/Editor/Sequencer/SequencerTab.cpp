#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerTab.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QScrollBar>
#include <QSlider>
#include <QVBoxLayout>
#include <utility>
#include <QUndoStack>

#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Sequencer/SequenceAsset.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorIcons.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Sequencer/SequencerAssets.h"
#include "Private/Editor/Sequencer/SequencerCurveEditor.h"
#include "Private/Editor/Sequencer/SequencerDetails.h"
#include "Private/Editor/Sequencer/SequencerDocument.h"
#include "Private/Editor/Sequencer/SequencerTimeline.h"

using Sequencer::ESequenceTrackType;

namespace
{
    /* The viewport camera that looks along a pose (ViewportWidget::ApplyGameCameraView's mapping). */
    void CameraFromPose(const GameCore::TransformPose& InPose, const float InFovY, EditorCamera& OutCamera)
    {
        OutCamera.Position = InPose.Position;
        OutCamera.Pitch    = std::clamp(-float(InPose.Rotation.GetX()), -89.0f, 89.0f);
        OutCamera.Yaw      = float(InPose.Rotation.GetY());
        OutCamera.FovY     = InFovY;
    }

    bool SameCamera(const EditorCamera& InA, const EditorCamera& InB)
    {
        const auto& A = InA.Position;
        const auto& B = InB.Position;
        return std::fabs(A.GetX() - B.GetX()) < 1e-3 && std::fabs(A.GetY() - B.GetY()) < 1e-3 && std::fabs(A.GetZ() - B.GetZ()) < 1e-3
            && std::fabs(InA.Yaw - InB.Yaw) < 1e-3f && std::fabs(InA.Pitch - InB.Pitch) < 1e-3f;
    }
}

SequencerTab::SequencerTab(EditorContext& InContext, QWidget* InParent)
    : QMainWindow(InParent), Context(InContext)
{
    setWindowFlags(Qt::Widget);
    Document = new SequencerDocument(this);
    Timeline = new SequencerTimeline(*Document, this);
    Curves   = new SequencerCurveEditor(*Document, this);
    Details  = new SequencerDetails(*Document, this);

    /* The timeline, with its time scrollbar under it. */
    auto* TimelineArea   = new QWidget(this);
    auto* TimelineLayout = new QVBoxLayout(TimelineArea);
    TimelineLayout->setContentsMargins(0, 0, 0, 0);
    TimelineLayout->setSpacing(0);
    TimelineLayout->addWidget(Timeline, 1);
    TimeScroll = new QScrollBar(Qt::Horizontal, TimelineArea);
    TimeScroll->setObjectName(QStringLiteral("SequencerTimeScroll"));
    TimelineLayout->addWidget(TimeScroll);

    auto* Lanes = new QSplitter(Qt::Vertical, this);
    Lanes->addWidget(TimelineArea);
    Lanes->addWidget(Curves);
    Lanes->setStretchFactor(0, 3);
    Lanes->setStretchFactor(1, 2);
    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Lanes);
    Split->addWidget(Details);
    Split->setStretchFactor(0, 4);
    Split->setStretchFactor(1, 1);
    Details->setMinimumWidth(300);
    Split->setSizes({ 1100, 340 });
    setCentralWidget(Split);

    PlayTimer = new QTimer(this);
    PlayTimer->setInterval(16);
    connect(PlayTimer, &QTimer::timeout, this, &SequencerTab::OnPlayTick);

    /* Auto-key waits for a move to settle: the gizmo's drag to end, a pilot to stop. */
    AutoKeyTimer = new QTimer(this);
    AutoKeyTimer->setSingleShot(true);
    AutoKeyTimer->setInterval(300);
    connect(AutoKeyTimer, &QTimer::timeout, this, &SequencerTab::FlushAutoKey);

    BuildToolBar();
    connect(Timeline, &SequencerTimeline::ViewChanged, this, &SequencerTab::SyncViewControls);
    connect(Document, &SequencerDocument::Changed, this, &SequencerTab::SyncViewControls);
    connect(TimeScroll, &QScrollBar::valueChanged, this, [this](const int InValue)
    {
        if(!bSyncingView) { Timeline->SetScrollFrame(double(InValue)); }
    });
    SyncViewControls();

    connect(Document, &SequencerDocument::Changed,         this, &SequencerTab::RefreshPreview);
    connect(Document, &SequencerDocument::LiveChanged,     this, &SequencerTab::RefreshPreview);
    connect(Document, &SequencerDocument::PlayheadChanged, this, [this]() { RefreshPreview(); UpdateTimecode(); });
    connect(&Document->GetHistory(), &QUndoStack::cleanChanged, this, &SequencerTab::TitleChanged);
    connect(&Document->GetHistory(), &QUndoStack::indexChanged, this, &SequencerTab::TitleChanged);
    connect(Timeline, &SequencerTimeline::ContextMenuRequested, this, &SequencerTab::ShowContextMenu);
    connect(Details, &SequencerDetails::BindToSelectedRequested, this, &SequencerTab::BindToSelected);

    /* The level the preview shows on. */
    EditorDocument& Level = Context.GetLevel();
    connect(&Level, &EditorDocument::AboutToSaveWorld, this, [this]()
    {
        if(Preview) { Preview->RestoreState(); }
    });
    connect(&Level, &EditorDocument::WorldSaved, this, [this]()
    {
        if(Preview) { Preview->Evaluate(Document->GetPlayhead()); }
    });
    connect(&Level, &EditorDocument::EditedWorldAboutToBeDestroyed, this, [this]()
    {
        /* The world goes with what was written into it: nothing to put back. */
        if(Preview)
        {
            Preview->DiscardState();
            Sequencer::PushPostProcess(Context.GetLevel().GetEditedWorld(), Sequencer::SequencePostProcess());
        }
        Preview.reset();
        PreviewAsset.reset();
    });
    connect(&Level, &EditorDocument::EditedWorldCreated, this, &SequencerTab::RefreshPreview);
    connect(&Level, &EditorDocument::PropertyEdited, this, &SequencerTab::OnPropertyEdited);
    connect(&Context, &EditorContext::PlayStateChanged, this, [this](const bool bInPlaying)
    {
        if(bInPlaying) { SetPlaying(false); ReleasePreview(); }
        else           { RefreshPreview(); }
    });
    connect(&Context, &EditorContext::BeforeFrame, this, &SequencerTab::OnBeforeFrame);
    if(ViewportWidget* Viewport = EditorSequencer::GetLevelViewport())
    {
        connect(Viewport, &ViewportWidget::GizmoDragStatus, this, [this](const QString& InText)
        {
            bGizmoDragging = !InText.isEmpty();
            if(!bGizmoDragging && !PendingKeys.empty()) { AutoKeyTimer->start(0); }
        });
    }
    UpdateTimecode();
}

SequencerTab::~SequencerTab()
{
    SetViewLocked(false);
    ReleasePreview();
}

// === The file ====================================================================

bool SequencerTab::Load(const QString& InPath, QString& OutError)
{
    if(!Document->Load(InPath, OutError))
    {
        return false;
    }
    Path = InPath;
    Timeline->FitAll();
    emit TitleChanged();
    return true;
}

QString SequencerTab::GetRuntimePath() const
{
    return EditorContent::ToRuntimePath(Path);
}

QString SequencerTab::GetTabTitle() const
{
    return QFileInfo(Path).completeBaseName() + (IsDirty() ? QStringLiteral("*") : QString());
}

bool SequencerTab::IsDirty() const
{
    return Document->IsDirty();
}

bool SequencerTab::Save()
{
    if(EditorContent::IsEngineContent(Path))
    {
        QMessageBox::warning(this, QStringLiteral("Save Sequence"), QStringLiteral("The engine's content is read-only here."));
        return false;
    }
    QString Error;
    if(!Document->Save(Path, Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save Sequence"), Error);
        return false;
    }
    /* Beside the exe, and out of the cache: the next Play reads the file again. */
    EditorContent::StageAsset(Path);
    Sequencer::SequenceAssetCache::Invalidate(GetRuntimePath().toStdString());
    emit TitleChanged();
    return true;
}

bool SequencerTab::ConfirmClose()
{
    if(IsDirty())
    {
        const auto Answer = QMessageBox::question(this, QStringLiteral("Sequencer"),
                                                  QStringLiteral("Save changes to %1?").arg(QFileInfo(Path).fileName()),
                                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if(Answer == QMessageBox::Cancel) { return false; }
        if(Answer == QMessageBox::Save && !Save()) { return false; }
    }
    SetPlaying(false);
    SetViewLocked(false);
    ReleasePreview();
    return true;
}

// === The toolbar =================================================================

void SequencerTab::BuildToolBar()
{
    using EditorIcons::EIcon;
    QToolBar* Bar = addToolBar(QStringLiteral("Sequencer"));
    Bar->setObjectName(QStringLiteral("SequencerToolBar"));
    Bar->setMovable(false);
    Bar->setIconSize(QSize(18, 18));
    Bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    /* A button, its icon, its tooltip and its shortcut (in this tab only). */
    const auto Add = [this, Bar](const EIcon InIcon, const QString& InText, const QString& InTip, const QKeySequence& InKey,
                                 auto&& InSlot) -> QAction*
    {
        QAction* Action = Bar->addAction(EditorIcons::Get(InIcon), InText);
        connect(Action, &QAction::triggered, this, std::forward<decltype(InSlot)>(InSlot));
        Action->setToolTip(InKey.isEmpty() ? InTip : QStringLiteral("%1 (%2)").arg(InTip, InKey.toString(QKeySequence::NativeText)));
        if(!InKey.isEmpty())
        {
            Action->setShortcut(InKey);
            Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        }
        return Action;
    };
    /* A toggle that keeps its label beside the icon: its state is worth reading. */
    const auto Labelled = [Bar](QAction* InAction)
    {
        if(auto* Button = qobject_cast<QToolButton*>(Bar->widgetForAction(InAction))) { Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); }
    };

    Add(EIcon::Save, QStringLiteral("Save"), QStringLiteral("Save the sequence"), QKeySequence::Save, [this]() { Save(); });
    UndoAction = Add(EIcon::Undo, QStringLiteral("Undo"), QStringLiteral("Undo"), QKeySequence::Undo, [this]() { Document->GetHistory().undo(); });
    RedoAction = Add(EIcon::Redo, QStringLiteral("Redo"), QStringLiteral("Redo"), QKeySequence::Redo, [this]() { Document->GetHistory().redo(); });
    const auto UpdateHistory = [this]()
    {
        QUndoStack& History = Document->GetHistory();
        UndoAction->setEnabled(History.canUndo());
        RedoAction->setEnabled(History.canRedo());
        UndoAction->setToolTip(History.canUndo() ? QStringLiteral("Undo %1 (Ctrl+Z)").arg(History.undoText()) : QStringLiteral("Undo (Ctrl+Z)"));
        RedoAction->setToolTip(History.canRedo() ? QStringLiteral("Redo %1 (Ctrl+Shift+Z)").arg(History.redoText()) : QStringLiteral("Redo (Ctrl+Shift+Z)"));
    };
    connect(&Document->GetHistory(), &QUndoStack::indexChanged, this, UpdateHistory);
    UpdateHistory();
    Bar->addSeparator();

    Add(EIcon::SkipBack, QStringLiteral("Start"), QStringLiteral("To the start"), QKeySequence(Qt::Key_Home),
        [this]() { SetPlaying(false); Document->SetPlayhead(0.0); });
    Add(EIcon::StepBack, QStringLiteral("Back"), QStringLiteral("Back a frame"), QKeySequence(Qt::Key_Comma), [this]() { StepFrames(-1); });
    PlayAction = Add(EIcon::Play, QStringLiteral("Play"), QStringLiteral("Play / pause the preview"), QKeySequence(Qt::Key_Space),
                     [this]() { SetPlaying(!bPlaying); });
    Add(EIcon::StepForward, QStringLiteral("Forward"), QStringLiteral("Forward a frame"), QKeySequence(Qt::Key_Period), [this]() { StepFrames(1); });
    Add(EIcon::SkipForward, QStringLiteral("End"), QStringLiteral("To the end"), QKeySequence(Qt::Key_End),
        [this]() { SetPlaying(false); Document->SetPlayhead(double(Document->GetAsset().Length)); });
    LoopAction = Add(EIcon::Loop, QStringLiteral("Loop"), QStringLiteral("Loop the preview"), QKeySequence(), []() {});
    LoopAction->setCheckable(true);
    LoopAction->setChecked(true);

    Timecode = new QLabel(Bar);
    Timecode->setObjectName(QStringLiteral("SequencerTimecode"));
    Timecode->setMinimumWidth(150);
    Timecode->setAlignment(Qt::AlignCenter);
    QFont Mono = Timecode->font();
    Mono.setFamily(QStringLiteral("monospace"));
    Mono.setStyleHint(QFont::Monospace);
    Timecode->setFont(Mono);
    Bar->addWidget(Timecode);
    Bar->addSeparator();

    AutoKeyAction = Add(EIcon::Record, QStringLiteral("Auto-key"), QStringLiteral("Moving a bound entity keys it at the playhead"), QKeySequence(),
                        [this](const bool bInOn) { SetAutoKey(bInOn); });
    AutoKeyAction->setCheckable(true);
    Labelled(AutoKeyAction);
    Add(EIcon::Key, QStringLiteral("Key"), QStringLiteral("Key the current binding's pose at the playhead"), QKeySequence(Qt::Key_K),
        [this]() { KeyCurrentBinding(); });
    LockAction = Add(EIcon::CameraCuts, QStringLiteral("Camera Cuts"), QStringLiteral("The level viewport looks through the shot at the playhead"),
                     QKeySequence(), [this](const bool bInOn) { SetViewLocked(bInOn); });
    LockAction->setCheckable(true);
    Labelled(LockAction);
    PilotAction = Add(EIcon::Pilot, QStringLiteral("Pilot"),
                      QStringLiteral("Flying the level viewport moves the shot's camera (keyed when it settles, with Auto-key)"), QKeySequence(),
                      [this](const bool bInOn) { SetPilot(bInOn); });
    PilotAction->setCheckable(true);
    Labelled(PilotAction);
    QAction* Snap = Add(EIcon::Snap, QStringLiteral("Snap"), QStringLiteral("Scrubbing lands on whole frames"), QKeySequence(),
                        [this](const bool bInOn) { Timeline->SetSnap(bInOn); });
    Snap->setCheckable(true);
    Snap->setChecked(true);
    Bar->addSeparator();

    /* + Binding and + Track, as menus. */
    auto* AddBinding = new QToolButton(Bar);
    AddBinding->setText(QStringLiteral("Binding"));
    AddBinding->setIcon(EditorIcons::Get(EIcon::Add));
    AddBinding->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    AddBinding->setToolTip(QStringLiteral("Add a binding: the level's selected entity, or a spawned prefab"));
    AddBinding->setPopupMode(QToolButton::InstantPopup);
    auto* BindingMenu = new QMenu(AddBinding);
    BindingMenu->addAction(QStringLiteral("Selected Entity"), this, [this]() { AddSelectedEntity(); });
    BindingMenu->addAction(QStringLiteral("Spawned Prefab..."), this, [this]()
    {
        const QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Spawn Prefab"), EditorGraphs::GetProjectRoot(),
                                                          QStringLiteral("Prefabs (*.nmprefab)"));
        const QString Runtime = File.isEmpty() ? QString() : EditorContent::ToRuntimePath(File);
        if(!Runtime.isEmpty()) { Document->AddSpawnBinding(QFileInfo(File).completeBaseName(), Runtime); }
    });
    AddBinding->setMenu(BindingMenu);
    Bar->addWidget(AddBinding);

    auto* AddTrack = new QToolButton(Bar);
    AddTrack->setText(QStringLiteral("Track"));
    AddTrack->setIcon(EditorIcons::Get(EIcon::Add));
    AddTrack->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    AddTrack->setToolTip(QStringLiteral("Add a track to the current binding, or to the sequence"));
    AddTrack->setPopupMode(QToolButton::InstantPopup);
    auto* TrackMenu = new QMenu(AddTrack);
    connect(TrackMenu, &QMenu::aboutToShow, this, [this, TrackMenu]()
    {
        TrackMenu->clear();
        FillAddTrackMenu(*TrackMenu, Document->GetCurrentBinding());
    });
    AddTrack->setMenu(TrackMenu);
    Bar->addWidget(AddTrack);
    Bar->addSeparator();

    /* Zoom: out, a slider (logarithmic, frames per pixel), in, and fit. */
    Add(EIcon::ZoomOut, QStringLiteral("Zoom Out"), QStringLiteral("Zoom the timeline out"), QKeySequence(Qt::CTRL | Qt::Key_Minus),
        [this]() { Timeline->ZoomBy(1.0 / 1.5); });
    ZoomSlider = new QSlider(Qt::Horizontal, Bar);
    ZoomSlider->setObjectName(QStringLiteral("SequencerZoom"));
    ZoomSlider->setRange(0, 1000);
    ZoomSlider->setFixedWidth(120);
    ZoomSlider->setToolTip(QStringLiteral("Timeline zoom (Ctrl+wheel, or the wheel over the ruler)"));
    Bar->addWidget(ZoomSlider);
    Add(EIcon::ZoomIn, QStringLiteral("Zoom In"), QStringLiteral("Zoom the timeline in"), QKeySequence(Qt::CTRL | Qt::Key_Equal),
        [this]() { Timeline->ZoomBy(1.5); });
    Add(EIcon::Fit, QStringLiteral("Fit"), QStringLiteral("The whole sequence across the timeline and curves"), QKeySequence(Qt::CTRL | Qt::Key_0),
        [this]() { Timeline->FitAll(); Curves->FitView(); });

    connect(ZoomSlider, &QSlider::valueChanged, this, [this](const int InValue)
    {
        if(bSyncingView) { return; }
        const double Range = std::log(SequencerTimeline::MAX_PIXELS_PER_FRAME / SequencerTimeline::MIN_PIXELS_PER_FRAME);
        const double Pixels = SequencerTimeline::MIN_PIXELS_PER_FRAME * std::exp(Range * InValue / 1000.0);
        Timeline->ZoomBy(Pixels / Timeline->GetPixelsPerFrame());
    });
}

void SequencerTab::SyncViewControls()
{
    bSyncingView = true;
    const double Range = std::log(SequencerTimeline::MAX_PIXELS_PER_FRAME / SequencerTimeline::MIN_PIXELS_PER_FRAME);
    ZoomSlider->setValue(int(std::lround(1000.0 * std::log(Timeline->GetPixelsPerFrame() / SequencerTimeline::MIN_PIXELS_PER_FRAME) / Range)));

    /* The scrollbar in frames: the sequence plus half a view of margin either side. */
    const double Visible = Timeline->GetVisibleFrames();
    const double Length  = double(std::max(Document->GetAsset().Length, 1));
    TimeScroll->setRange(int(std::floor(-Visible * 0.5)), int(std::ceil(std::max(Length - Visible * 0.5, -Visible * 0.5))));
    TimeScroll->setPageStep(std::max(1, int(Visible)));
    TimeScroll->setSingleStep(std::max(1, int(Visible / 20)));
    TimeScroll->setValue(int(std::lround(Timeline->GetScrollFrame())));
    bSyncingView = false;
}

void SequencerTab::FillAddTrackMenu(QMenu& InMenu, const int InBinding)
{
    const auto Add = [this, &InMenu](const QString& InText, const int InOn, const ESequenceTrackType InType)
    {
        InMenu.addAction(InText, this, [this, InOn, InType]() { AddTrackPrompted(InOn, InType); });
    };
    if(InBinding >= 0)
    {
        const auto& Binding = Document->GetAsset().Bindings[size_t(InBinding)];
        InMenu.addSection(QStringLiteral("On %1").arg(QString::fromStdString(Binding.Name.empty() ? Binding.Id : Binding.Name)));
        Add(QStringLiteral("Transform"), InBinding, ESequenceTrackType::Transform);
        Add(QStringLiteral("Animation"), InBinding, ESequenceTrackType::Animation);
        Add(QStringLiteral("Audio"), InBinding, ESequenceTrackType::Audio);
        Add(QStringLiteral("Event"), InBinding, ESequenceTrackType::Event);
        Add(QStringLiteral("Number Field..."), InBinding, ESequenceTrackType::Float);
        Add(QStringLiteral("Flag Field..."), InBinding, ESequenceTrackType::Bool);
    }
    InMenu.addSection(QStringLiteral("On the sequence"));
    Add(QStringLiteral("Camera Cut"), -1, ESequenceTrackType::CameraCut);
    Add(QStringLiteral("Fade"), -1, ESequenceTrackType::Fade);
    Add(QStringLiteral("Audio"), -1, ESequenceTrackType::Audio);
    Add(QStringLiteral("Event"), -1, ESequenceTrackType::Event);
    Add(QStringLiteral("Sub-Sequence"), -1, ESequenceTrackType::SubSequence);
    Add(QStringLiteral("Post Process"), -1, ESequenceTrackType::PostProcess);
    Add(QStringLiteral("Post Process Setting..."), -1, ESequenceTrackType::PostProcessSetting);
}

void SequencerTab::AddTrackPrompted(const int InBinding, const ESequenceTrackType InType)
{
    if(InType == ESequenceTrackType::PostProcessSetting)
    {
        QStringList Settings;
        for(size_t Index = 0; Index < size_t(RenderCore::EPostProcessParam::Count); ++Index)
        {
            Settings << QString::fromLatin1(RenderCore::PostProcessParamName(RenderCore::EPostProcessParam(Index)));
        }
        bool bOk = false;
        const QString Setting = QInputDialog::getItem(this, QStringLiteral("Add Post Process Setting"), QStringLiteral("Setting:"), Settings, 0, false, &bOk);
        if(bOk && !Setting.isEmpty()) { Document->AddTrack(-1, InType, QString(), Setting); }
        return;
    }
    if(InType == ESequenceTrackType::Float || InType == ESequenceTrackType::Bool)
    {
        bool bOk = false;
        const QString Text = QInputDialog::getText(this, QStringLiteral("Add Field Track"),
                                                   QStringLiteral("Component and field, as Lua names them (Camera.FovY):"),
                                                   QLineEdit::Normal, QString(), &bOk).trimmed();
        const int Dot = Text.lastIndexOf(QLatin1Char('.'));
        if(!bOk || Dot <= 0 || Dot == Text.size() - 1) { return; }
        Document->AddTrack(InBinding, InType, Text.left(Dot), Text.mid(Dot + 1));
        return;
    }
    Document->AddTrack(InBinding, InType);
}

void SequencerTab::ShowContextMenu(const int InRow, const double InFrame, const QPoint& InGlobalPos)
{
    QMenu Menu(this);
    const int Frame = int(std::lround(InFrame));
    if(InRow < 0)
    {
        Menu.addAction(QStringLiteral("Add Marker Here"), this, [this, Frame]()
        {
            bool bOk = false;
            const QString Name = QInputDialog::getText(this, QStringLiteral("Add Marker"), QStringLiteral("Name:"), QLineEdit::Normal,
                                                       QStringLiteral("Marker"), &bOk).trimmed();
            if(bOk && !Name.isEmpty()) { Document->AddMarker(Frame, Name); }
        });
        Menu.addAction(QStringLiteral("Set End Here"), this, [this, Frame]()
        {
            Document->Edit(QStringLiteral("Set Length"), [Frame](Sequencer::SequenceAsset& InOut) { InOut.Length = std::max(1, Frame); });
        });
        Menu.exec(InGlobalPos);
        return;
    }

    const auto& Rows = Timeline->GetRows();
    const SequencerTimeline::Row Row = Rows[size_t(InRow)];
    if(!Document->GetSelection().empty())
    {
        Menu.addAction(QStringLiteral("Delete Selected"), this, [this]() { Document->DeleteSelection(); });
        Menu.addSeparator();
    }
    if(Row.Kind == SequencerTimeline::Row::EKind::Binding)
    {
        QMenu* Tracks = Menu.addMenu(QStringLiteral("Add Track"));
        FillAddTrackMenu(*Tracks, Row.Binding);
        Menu.addAction(QStringLiteral("Bind to Selected Entity"), this, [this, Row]() { BindToSelected(Row.Binding); });
        Menu.addAction(QStringLiteral("Key Pose Here"), this, [this, Row, Frame]()
        {
            Document->SetPlayhead(Frame);
            Document->SetCurrent(Row.Binding, SequencerTrackRef());
            KeyCurrentBinding();
        });
        Menu.addSeparator();
        Menu.addAction(QStringLiteral("Remove Binding"), this, [this, Row]() { Document->RemoveBinding(Row.Binding); });
    }
    else if(Row.Kind == SequencerTimeline::Row::EKind::Header)
    {
        QMenu* Tracks = Menu.addMenu(QStringLiteral("Add Track"));
        FillAddTrackMenu(*Tracks, -1);
    }
    else if(Row.Track.IsValid())
    {
        const Sequencer::SequenceTrack* Track = Document->FindTrack(Row.Track);
        if(Track)
        {
            Menu.addAction(Track->bMuted ? QStringLiteral("Unmute") : QStringLiteral("Mute"), this, [this, Row]()
            {
                Document->Edit(QStringLiteral("Mute Track"), [this, Row](Sequencer::SequenceAsset&)
                {
                    if(auto* Live = Document->FindLiveTrack(Row.Track)) { Live->bMuted = !Live->bMuted; }
                });
            });
            if(Track->Type == ESequenceTrackType::Event)
            {
                Menu.addAction(QStringLiteral("Add Event Here"), this, [this, Row, Frame]() { Document->AddEvent(Row.Track, Frame, QStringLiteral("Event")); });
            }
            if(Sequencer::IsSectionTrack(Track->Type))
            {
                Menu.addAction(QStringLiteral("Add Section Here"), this, [this, Row, Frame]()
                {
                    const int Length = int(std::max<uint32_t>(Document->GetAsset().FrameRate, 1)) * 2;
                    Document->AddSection(Row.Track, Frame, Frame + Length, QString());
                });
            }
            Menu.addSeparator();
            Menu.addAction(QStringLiteral("Remove Track"), this, [this, Row]() { Document->RemoveTrack(Row.Track); });
        }
    }
    Menu.exec(InGlobalPos);
}

void SequencerTab::UpdateTimecode()
{
    const Sequencer::SequenceAsset& Asset = Document->GetAsset();
    const int Fps   = int(std::max<uint32_t>(Asset.FrameRate, 1));
    const int Frame = int(std::lround(Document->GetPlayhead()));
    Timecode->setText(QStringLiteral("%1:%2  /  %3 f").arg(Frame / Fps, 2, 10, QLatin1Char('0')).arg(Frame % Fps, 2, 10, QLatin1Char('0'))
                          .arg(Frame));
}

// === Playing =====================================================================

void SequencerTab::SetPlaying(const bool bInPlaying)
{
    if(bInPlaying == bPlaying) { return; }
    bPlaying = bInPlaying;
    if(PlayAction)
    {
        PlayAction->setIcon(EditorIcons::Get(bPlaying ? EditorIcons::EIcon::Pause : EditorIcons::EIcon::Play));
        PlayAction->setText(bPlaying ? QStringLiteral("Pause") : QStringLiteral("Play"));
    }
    if(bPlaying)
    {
        if(Document->GetPlayhead() >= double(Document->GetAsset().Length)) { Document->SetPlayhead(0.0); }
        PlayClock.start();
        PlayTimer->start();
    }
    else
    {
        PlayTimer->stop();
    }
}

void SequencerTab::OnPlayTick()
{
    const double Seconds = double(PlayClock.restart()) / 1000.0;
    const Sequencer::SequenceAsset& Asset = Document->GetAsset();
    double Frame = Document->GetPlayhead() + Seconds * double(Asset.FrameRate);
    if(Frame >= double(Asset.Length))
    {
        if(LoopAction->isChecked() && Asset.Length > 0)
        {
            Frame = std::fmod(Frame, double(Asset.Length));
        }
        else
        {
            Frame = double(Asset.Length);
            SetPlaying(false);
        }
    }
    Document->SetPlayhead(Frame);
}

void SequencerTab::StepFrames(const int InFrames)
{
    SetPlaying(false);
    Document->SetPlayhead(std::round(Document->GetPlayhead()) + InFrames);
}

// === The preview =================================================================

void SequencerTab::RefreshPreview()
{
    if(Context.IsPlaying())
    {
        ReleasePreview();
        return;
    }
    GameCore::World& World = Context.GetLevel().GetEditedWorld();

    /* A new asset: what the old one wrote goes back, the new one writes afresh. */
    std::shared_ptr<const Sequencer::SequenceAsset> Asset = Document->GetSnapshot();
    if(!Preview || Asset != PreviewAsset)
    {
        if(Preview)
        {
            Preview->RestoreState();
            Preview.reset();
        }
        Sequencer::SequenceInstanceOptions Options;
        Options.bAudio        = false;
        Options.bCameraEntity = false;
        Options.bSpawn        = false;
        Preview      = std::make_unique<Sequencer::SequenceInstance>(World, Asset, GetRuntimePath().toStdString(), Options);
        PreviewAsset = Asset;
    }
    Preview->Evaluate(Document->GetPlayhead());
    World.RefreshSpatialIndex();
    Sequencer::PushPostProcess(World, Preview->GetPostProcess());
    RememberShownPoses();
}

void SequencerTab::ReleasePreview()
{
    if(!Preview) { return; }
    Preview->RestoreState();
    Preview->Release();
    Sequencer::PushPostProcess(Context.GetLevel().GetEditedWorld(), Sequencer::SequencePostProcess());
    Preview.reset();
    PreviewAsset.reset();
    PendingKeys.clear();
}

void SequencerTab::RememberShownPoses()
{
    const size_t Count = Document->GetAsset().Bindings.size();
    Shown.assign(Count, GameCore::TransformPose());
    bShown.assign(Count, false);
    if(!Preview) { return; }
    GameCore::World& World = Context.GetLevel().GetEditedWorld();
    for(size_t Index = 0; Index < Count; ++Index)
    {
        const GameCore::EntityID Entity = Preview->ResolveBinding(Index);
        if(!Entity.IsSet() || !World.IsAlive(Entity)) { continue; }
        if(const GameCore::TransformComponent* Transform = World.GetEntity(Entity).GetComponent<GameCore::TransformComponent>())
        {
            Shown[Index]  = Transform->GetPose();
            bShown[Index] = true;
        }
    }
}

int SequencerTab::BindingOf(const GameCore::EntityID InEntity)
{
    if(!Preview || !InEntity.IsSet()) { return -1; }
    for(size_t Index = 0; Index < Document->GetAsset().Bindings.size(); ++Index)
    {
        if(Preview->ResolveBinding(Index) == InEntity) { return int(Index); }
    }
    return -1;
}

// === Keys ========================================================================

void SequencerTab::OnPropertyEdited(const GameCore::EntityID InEntity, const uint16_t InType)
{
    if(!bAutoKey || !Preview || InType != GameCore::TransformComponent::StaticType().ID) { return; }
    const int Binding = BindingOf(InEntity);
    if(Binding < 0) { return; }
    PendingKeys.insert(Binding);
    /* Mid-drag, the drag's end flushes; otherwise this edit, once settled. */
    if(!bGizmoDragging) { AutoKeyTimer->start(0); }
}

void SequencerTab::FlushAutoKey()
{
    if(!Preview || PendingKeys.empty()) { PendingKeys.clear(); return; }
    GameCore::World& World = Context.GetLevel().GetEditedWorld();
    const std::set<int> Pending = std::move(PendingKeys);
    PendingKeys.clear();
    const int Frame = int(std::lround(Document->GetPlayhead()));
    for(const int Binding : Pending)
    {
        if(size_t(Binding) >= bShown.size() || !bShown[size_t(Binding)]) { continue; }
        const GameCore::EntityID Entity = Preview->ResolveBinding(size_t(Binding));
        if(!World.IsAlive(Entity)) { continue; }
        const GameCore::TransformComponent* Transform = World.GetEntity(Entity).GetComponent<GameCore::TransformComponent>();
        if(!Transform) { continue; }
        Document->KeyTransform(Binding, Shown[size_t(Binding)], Transform->GetPose(), Frame);
    }
}

void SequencerTab::KeyCurrentBinding()
{
    const int Binding = Document->GetCurrentBinding();
    if(Binding < 0 || !Preview) { return; }
    GameCore::World& World = Context.GetLevel().GetEditedWorld();
    const GameCore::EntityID Entity = Preview->ResolveBinding(size_t(Binding));
    if(!Entity.IsSet() || !World.IsAlive(Entity)) { return; }
    const GameCore::TransformComponent* Transform = World.GetEntity(Entity).GetComponent<GameCore::TransformComponent>();
    if(!Transform) { return; }
    const GameCore::TransformPose Pose = Transform->GetPose();
    Document->KeyTransform(Binding, size_t(Binding) < bShown.size() && bShown[size_t(Binding)] ? Shown[size_t(Binding)] : Pose, Pose,
                           int(std::lround(Document->GetPlayhead())), true);
}

int SequencerTab::AddSelectedEntity()
{
    EditorDocument& Level = Context.GetLevel();
    const GameCore::EntityID Selected = Level.GetSelection();
    GameCore::World& World = Level.GetEditedWorld();
    if(!Selected.IsSet() || !World.IsAlive(Selected) || Context.IsPlaying())
    {
        QMessageBox::information(this, QStringLiteral("Add Binding"), QStringLiteral("Select an entity in the level first."));
        return -1;
    }
    const GameCore::Entity Entity = World.GetEntity(Selected);
    const int Index = Document->AddPossessBinding(QString::fromUtf8(Entity.GetName()), Entity.GetGuid());
    if(Index >= 0) { Document->SetCurrent(Index, SequencerTrackRef()); }
    return Index;
}

void SequencerTab::BindToSelected(const int InBinding)
{
    EditorDocument& Level = Context.GetLevel();
    const GameCore::EntityID Selected = Level.GetSelection();
    GameCore::World& World = Level.GetEditedWorld();
    if(!Selected.IsSet() || !World.IsAlive(Selected) || InBinding < 0) { return; }
    const GameCore::Entity Entity = World.GetEntity(Selected);
    const GameCore::EntityGuid Guid = Entity.GetGuid();
    const std::string Name = Entity.GetName();
    Document->Edit(QStringLiteral("Bind to Selected Entity"), [&](Sequencer::SequenceAsset& InOut)
    {
        auto& Binding = InOut.Bindings[size_t(InBinding)];
        Binding.Kind = Sequencer::ESequenceBindingKind::Possess;
        Binding.Guid = Guid;
        Binding.Name = Name;
    });
}

void SequencerTab::SetAutoKey(const bool bInOn)
{
    bAutoKey = bInOn;
    if(AutoKeyAction && AutoKeyAction->isChecked() != bInOn) { AutoKeyAction->setChecked(bInOn); }
    if(!bInOn) { PendingKeys.clear(); }
}

// === The viewport ================================================================

void SequencerTab::SetViewLocked(const bool bInOn)
{
    bViewLocked = bInOn;
    if(LockAction && LockAction->isChecked() != bInOn) { LockAction->setChecked(bInOn); }
    if(!bInOn)
    {
        if(bPilot) { SetPilot(false); }
        if(bCameraSaved)
        {
            if(ViewportWidget* Viewport = EditorSequencer::GetLevelViewport()) { Viewport->GetCamera() = SavedCamera; }
            bCameraSaved = false;
        }
        bLastSetValid = false;
    }
}

void SequencerTab::SetPilot(const bool bInOn)
{
    bPilot = bInOn;
    if(PilotAction && PilotAction->isChecked() != bInOn) { PilotAction->setChecked(bInOn); }
    if(bInOn && !bViewLocked) { SetViewLocked(true); }
}

void SequencerTab::OnBeforeFrame()
{
    if(!bViewLocked || !Preview || Context.IsPlaying()) { return; }
    ViewportWidget* Viewport = EditorSequencer::GetLevelViewport();
    const Sequencer::SequenceShot& Shot = Preview->GetShot();
    if(!Viewport || !Shot.bValid) { return; }
    EditorCamera& Camera = Viewport->GetCamera();
    if(!bCameraSaved)
    {
        SavedCamera  = Camera;
        bCameraSaved = true;
    }

    /* Piloting: the user flew the viewport since the shot set it -- the
     * shot's camera follows, keyed once it settles. */
    if(bPilot && bLastSetValid && Shot.Blend >= 1.0f && !SameCamera(Camera, LastSetCamera))
    {
        GameCore::World& World = Context.GetLevel().GetEditedWorld();
        if(World.IsAlive(Shot.To))
        {
            GameCore::TransformPose Pose = World.GetEntity(Shot.To).GetWorldPose();
            Pose.Position = Camera.Position;
            Pose.Rotation = EngineCore::Vector3(-Camera.Pitch, Camera.Yaw, 0.0);
            World.GetEntity(Shot.To).SetWorldPose(Pose);
            const int Binding = BindingOf(Shot.To);
            if(bAutoKey && Binding >= 0)
            {
                PendingKeys.insert(Binding);
                AutoKeyTimer->start();
            }
            LastSetCamera = Camera;
            return;
        }
    }
    CameraFromPose(Shot.Pose, Shot.FovY, Camera);
    LastSetCamera = Camera;
    bLastSetValid = true;
}

#endif // WITH_EDITOR
