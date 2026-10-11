#if WITH_EDITOR

#include "Private/Editor/Content/AnimStateEditorWindow.h"

#include <algorithm>
#include <map>
#include <memory>

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"
#include "Private/Editor/Content/AnimStateGraph.h"
#include "Private/Editor/Content/AnimationAssets.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace
{
    QList<QPointer<AnimStateEditorWindow>>& OpenEditors()
    {
        static QList<QPointer<AnimStateEditorWindow>> Editors;
        return Editors;
    }

    const QStringList TYPE_NAMES    = { QStringLiteral("Float"), QStringLiteral("Bool"), QStringLiteral("Trigger") };
    const QStringList LOOP_NAMES    = { QStringLiteral("As the clip says"), QStringLiteral("Once"), QStringLiteral("Loop") };
    const QStringList COMPARE_NAMES = { QStringLiteral(">"), QStringLiteral("<"), QStringLiteral("=="), QStringLiteral("!="),
                                        QStringLiteral("is true"), QStringLiteral("is false"), QStringLiteral("triggered") };

    QString ToQ(const std::string& InText) { return QString::fromStdString(InText); }

    /* "+" and "-" under a list or table. */
    QHBoxLayout* AddRemoveRow(QWidget* InParent, const QString& InWhat, QPushButton*& OutAdd, QPushButton*& OutRemove)
    {
        OutAdd    = new QPushButton(QStringLiteral("+"), InParent);
        OutRemove = new QPushButton(QStringLiteral("-"), InParent);
        OutAdd->setToolTip(QStringLiteral("Add a %1").arg(InWhat));
        OutRemove->setToolTip(QStringLiteral("Remove the selected %1").arg(InWhat));
        OutAdd->setFixedWidth(32);
        OutRemove->setFixedWidth(32);
        auto* Row = new QHBoxLayout();
        Row->setContentsMargins(0, 0, 0, 0);
        Row->addWidget(OutAdd);
        Row->addWidget(OutRemove);
        Row->addStretch(1);
        return Row;
    }

    QTableWidget* MakeTable(QWidget* InParent, const QStringList& InColumns)
    {
        auto* Table = new QTableWidget(0, int(InColumns.size()), InParent);
        Table->setHorizontalHeaderLabels(InColumns);
        Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        Table->verticalHeader()->setVisible(false);
        Table->setSelectionBehavior(QAbstractItemView::SelectRows);
        Table->setSelectionMode(QAbstractItemView::SingleSelection);
        Table->setMinimumHeight(110);
        return Table;
    }
}

// === Opening ==============================================================================

AnimStateEditorWindow* AnimStateEditorWindow::Open(const QString& InPath, QWidget* InParent)
{
    const QString Path = QFileInfo(InPath).absoluteFilePath();
    for(const QPointer<AnimStateEditorWindow>& Each : OpenEditors())
    {
        if(Each && Each->GetPath().compare(Path, Qt::CaseInsensitive) == 0)
        {
            AnimStateEditorWindow* Found = Each;
            OpenEditors().removeAll(Each);
            OpenEditors() << Found;
            Found->show();
            Found->raise();
            Found->activateWindow();
            return Found;
        }
    }

    auto* Editor = new AnimStateEditorWindow(Path);
    QString Error;
    if(!Editor->Load(Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("Open Anim State"), QStringLiteral("Could not open %1\n\n%2")
                             .arg(QFileInfo(Path).fileName(), Error));
        delete Editor;
        return nullptr;
    }
    Editor->BuildUi();
    OpenEditors().removeAll(nullptr);
    OpenEditors() << Editor;
    Editor->resize(1760, 920);
    Editor->show();
    return Editor;
}

AnimStateEditorWindow* AnimStateEditorWindow::FindFront()
{
    OpenEditors().removeAll(nullptr);
    return OpenEditors().isEmpty() ? nullptr : OpenEditors().back().data();
}

AnimStateEditorWindow::AnimStateEditorWindow(const QString& InPath)
    : QMainWindow(nullptr)
    , Path(InPath)
    , bReadOnly(EditorContent::IsEngineContent(InPath))
{
    setAttribute(Qt::WA_DeleteOnClose);
}

bool AnimStateEditorWindow::Load(QString& OutError)
{
    EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(Path);
    RenderCore::AnimStateData Loaded;
    if(!RenderCore::SerializeAnimState(Ar, Loaded))
    {
        OutError = QStringLiteral("Not an anim state this editor can read (the log says why)");
        return false;
    }
    Saved = Loaded;
    Edit  = std::move(Loaded);
    return true;
}

// === Building =============================================================================

void AnimStateEditorWindow::BuildUi()
{
    auto* Forms  = new QWidget(this);
    auto* Layout = new QVBoxLayout(Forms);
    Layout->addWidget(BuildParameters());
    Layout->addWidget(BuildLayers());
    Layout->addWidget(BuildLookAt());
    Layout->addWidget(BuildStates(), 1);
    Layout->addWidget(BuildTransitions(), 1);
    Layout->addStretch(0);

    SaveButton   = new QPushButton(QStringLiteral("Save"), Forms);
    RevertButton = new QPushButton(QStringLiteral("Revert"), Forms);
    SaveButton->setToolTip(bReadOnly ? QStringLiteral("The engine's content is read-only here") : QStringLiteral("Write the file and restage it (Ctrl+S)"));
    connect(SaveButton, &QPushButton::clicked, this, &AnimStateEditorWindow::Save);
    connect(RevertButton, &QPushButton::clicked, this, [this]()
    {
        Edit = Saved;
        Edited(true, /*bInDirty*/ false);
    });
    auto* SaveRow = new QHBoxLayout();
    SaveRow->addStretch(1);
    SaveRow->addWidget(RevertButton);
    SaveRow->addWidget(SaveButton);
    Layout->addLayout(SaveRow);

    auto* Scroll = new QScrollArea(this);
    Scroll->setWidget(Forms);
    Scroll->setWidgetResizable(true);

    /* The graph of the layer picked on the left; what is picked on the graph, edited on the left. */
    Graph = new AnimStateGraph(this);
    connect(Graph, &AnimStateGraph::StateSelected, this, [this](const int InState)
    {
        ShowDetail(false);
        StateList->setCurrentRow(InState);
    });
    connect(Graph, &AnimStateGraph::TransitionSelected, this, [this](const int InTransition)
    {
        ShowDetail(true);
        TransitionList->setCurrentRow(InTransition);
    });
    connect(Graph, &AnimStateGraph::StateMoved, this, [this](const int InState, const QPointF& InAt)
    {
        /* Only where it is drawn: nothing to replay, and the graph already shows it. */
        RenderCore::AnimLayer* Layer = CurrentLayer();
        if(!Layer || bReadOnly) { return; }
        if(InState < 0) { Layer->AnyEditorX = float(InAt.x()); Layer->AnyEditorY = float(InAt.y()); }
        else if(size_t(InState) < Layer->States.size())
        {
            Layer->States[size_t(InState)].EditorX = float(InAt.x());
            Layer->States[size_t(InState)].EditorY = float(InAt.y());
        }
        Layer->bEditorPlaced = true;
        bDirty = true;
        UpdateTitle();
    });
    /* Queued: each redraws the graph, which must not happen under its own event. */
    connect(Graph, &AnimStateGraph::AddStateRequested, this, &AnimStateEditorWindow::AddState, Qt::QueuedConnection);
    connect(Graph, &AnimStateGraph::RemoveStateRequested, this, &AnimStateEditorWindow::RemoveState, Qt::QueuedConnection);
    connect(Graph, &AnimStateGraph::AddTransitionRequested, this, &AnimStateEditorWindow::AddTransition, Qt::QueuedConnection);
    connect(Graph, &AnimStateGraph::RemoveTransitionRequested, this, &AnimStateEditorWindow::RemoveTransition, Qt::QueuedConnection);
    connect(Graph, &AnimStateGraph::EntryRequested, this, [this](const int InState)
    {
        RenderCore::AnimLayer* Layer = CurrentLayer();
        if(Layer && InState >= 0 && size_t(InState) < Layer->States.size()) { Layer->EntryState = InState; Edited(true); }
    }, Qt::QueuedConnection);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Scroll);
    Split->addWidget(Graph);
    Split->addWidget(BuildPreview());
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setStretchFactor(2, 0);
    Split->setSizes({ 560, 660, 540 });
    setCentralWidget(Split);

    auto* SaveKey = new QAction(this);
    SaveKey->setShortcut(QKeySequence::Save);
    connect(SaveKey, &QAction::triggered, this, &AnimStateEditorWindow::Save);
    addAction(SaveKey);

    /* A mesh on the clips' skeleton, if the project has one. */
    PickMesh(EditorAnimation::FindMeshOn(EditorAnimation::SkeletonOfAnimation(Path)));

    Edited(true, /*bInDirty*/ false);
}

QWidget* AnimStateEditorWindow::BuildParameters()
{
    auto* Box = new QGroupBox(QStringLiteral("Parameters -- what gameplay sets (animation:SetFloat, SetBool, SetTrigger)"), this);
    auto* Layout = new QVBoxLayout(Box);
    ParameterTable = MakeTable(Box, { QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("Default") });
    Layout->addWidget(ParameterTable);
    QPushButton *Add = nullptr, *Remove = nullptr;
    Layout->addLayout(AddRemoveRow(Box, QStringLiteral("parameter"), Add, Remove));

    connect(ParameterTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* InItem)
    {
        const int Row = InItem->row();
        if(bFilling || Row < 0 || size_t(Row) >= Edit.Parameters.size()) { return; }
        RenderCore::AnimStateParameter& Parameter = Edit.Parameters[size_t(Row)];
        if(InItem->column() == 0)
        {
            const std::string Name = InItem->text().trimmed().toStdString();
            if(Name.empty() || Name == Parameter.Name || Edit.FindParameter(Name) >= 0)
            {
                const QSignalBlocker Block(ParameterTable);
                InItem->setText(ToQ(Parameter.Name));   // empty or taken: refused
                return;
            }
            RenameParameter(Parameter.Name, Name);
            Edited(true);
        }
        else if(InItem->column() == 2)
        {
            Parameter.Default = InItem->text().toFloat();
            Edited();
        }
    });
    connect(Add, &QPushButton::clicked, this, [this]()
    {
        std::string Name = "Parameter";
        for(int Suffix = 2; Edit.FindParameter(Name) >= 0; ++Suffix) { Name = "Parameter" + std::to_string(Suffix); }
        Edit.Parameters.push_back({ Name, RenderCore::EAnimParameterType::Float, 0.0f });
        Edited(true);
        ParameterTable->setCurrentCell(ParameterTable->rowCount() - 1, 0);
    });
    connect(Remove, &QPushButton::clicked, this, [this]()
    {
        const int Row = ParameterTable->currentRow();
        if(Row < 0 || size_t(Row) >= Edit.Parameters.size()) { return; }
        Edit.Parameters.erase(Edit.Parameters.begin() + Row);   // what named it now names nothing: shown under Problems
        Edited(true);
    });
    return Box;
}

QWidget* AnimStateEditorWindow::BuildLayers()
{
    auto* Box = new QGroupBox(QStringLiteral("Layers -- each over the ones above it, through its mask"), this);
    auto* Row = new QHBoxLayout(Box);

    auto* ListColumn = new QVBoxLayout();
    LayerList = new QListWidget(Box);
    LayerList->setMaximumWidth(180);
    LayerList->setMinimumHeight(90);
    ListColumn->addWidget(LayerList);
    QPushButton *Add = nullptr, *Remove = nullptr;
    ListColumn->addLayout(AddRemoveRow(Box, QStringLiteral("layer"), Add, Remove));
    Row->addLayout(ListColumn);

    auto* Form = new QFormLayout();
    LayerName = new QLineEdit(Box);
    LayerWeight = new QDoubleSpinBox(Box);
    LayerWeight->setRange(0.0, 1.0);
    LayerWeight->setSingleStep(0.1);
    LayerWeightParameter = new QComboBox(Box);
    LayerWeightParameter->setToolTip(QStringLiteral("A float parameter (0..1) that scales the weight: Armed, say, to raise a weapon layer"));
    LayerMask = new QLineEdit(Box);
    LayerMask->setPlaceholderText(QStringLiteral("every bone"));
    LayerMask->setToolTip(QStringLiteral("Bones it moves, comma-separated, each with every bone below it (spine_02 for the upper body). "
                                         "Empty: all of them"));
    LayerEntry = new QComboBox(Box);
    LayerBlend = new QComboBox(Box);
    LayerBlend->addItems({ QStringLiteral("Override"), QStringLiteral("Additive") });
    LayerBlend->setToolTip(QStringLiteral("Override: its pose replaces what the layers above it make, by weight and mask. "
                                          "Additive: its clips' movement is added to it -- breathing, a flinch, a lean"));
    LayerBase = new QComboBox(Box);
    LayerBase->addItems({ QStringLiteral("Each clip's first frame"), QStringLiteral("Reference pose") });
    LayerBase->setToolTip(QStringLiteral("What an additive clip's movement is measured from: its own first frame (it adds how it "
                                         "moves from there), or the skeleton's reference pose (it adds its whole pose's difference)"));
    Form->addRow(QStringLiteral("Name"), LayerName);
    Form->addRow(QStringLiteral("Blend"), LayerBlend);
    Form->addRow(QStringLiteral("Additive Base"), LayerBase);
    Form->addRow(QStringLiteral("Weight"), LayerWeight);
    Form->addRow(QStringLiteral("Weight Parameter"), LayerWeightParameter);
    Form->addRow(QStringLiteral("Mask"), LayerMask);
    Form->addRow(QStringLiteral("Entry State"), LayerEntry);
    Row->addLayout(Form, 1);

    connect(LayerList, &QListWidget::currentRowChanged, this, [this](int) { if(!bFilling) { FillLayer(); } });
    connect(LayerName, &QLineEdit::editingFinished, this, [this]()
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer()) { Layer->Name = LayerName->text().trimmed().toStdString(); Edited(true); }
    });
    connect(LayerWeight, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer(); Layer && !bFilling) { Layer->Weight = float(InValue); Edited(); }
    });
    connect(LayerBlend, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer())
        {
            Layer->Blend = RenderCore::EAnimLayerBlend(InIndex);
            Edited();
            FillLayer();
        }
    });
    connect(LayerBase, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer()) { Layer->AdditiveBase = RenderCore::EAnimAdditiveBase(InIndex); Edited(); }
    });
    connect(LayerWeightParameter, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer())
        {
            Layer->WeightParameter = InIndex <= 0 ? std::string() : LayerWeightParameter->itemText(InIndex).toStdString();
            Edited();
        }
    });
    connect(LayerMask, &QLineEdit::editingFinished, this, [this]()
    {
        RenderCore::AnimLayer* Layer = CurrentLayer();
        if(!Layer) { return; }
        Layer->MaskBones.clear();
        for(const QString& Bone : LayerMask->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
        {
            if(!Bone.trimmed().isEmpty()) { Layer->MaskBones.push_back(Bone.trimmed().toStdString()); }
        }
        Edited();
    });
    connect(LayerEntry, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimLayer* Layer = CurrentLayer()) { Layer->EntryState = InIndex; Edited(true); }
    });
    connect(Add, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimLayer Layer;
        Layer.Name = Edit.Layers.empty() ? "Base" : "Layer " + std::to_string(Edit.Layers.size() + 1);
        RenderCore::AnimStateNode Idle;
        Idle.Name = "Idle";
        Layer.States.push_back(Idle);
        Edit.Layers.push_back(Layer);
        Edited(true);
        LayerList->setCurrentRow(LayerList->count() - 1);
    });
    connect(Remove, &QPushButton::clicked, this, [this]()
    {
        const int Row = LayerList->currentRow();
        if(Row < 0 || size_t(Row) >= Edit.Layers.size()) { return; }
        Edit.Layers.erase(Edit.Layers.begin() + Row);
        Edited(true);
    });
    return Box;
}

QWidget* AnimStateEditorWindow::BuildStates()
{
    auto* Box = new QGroupBox(QStringLiteral("State -- picked on the graph"), this);
    StateBox = Box;
    auto* Row = new QHBoxLayout(Box);

    /* The list and its buttons stay, hidden: the graph picks and adds. */
    auto* ListColumn = new QVBoxLayout();
    StateList = new QListWidget(Box);
    StateList->setMaximumWidth(180);
    ListColumn->addWidget(StateList);
    QPushButton *Add = nullptr, *Remove = nullptr;
    ListColumn->addLayout(AddRemoveRow(Box, QStringLiteral("state"), Add, Remove));
    Row->addLayout(ListColumn);
    for(QWidget* Each : std::initializer_list<QWidget*>{ StateList, Add, Remove }) { Each->hide(); }

    StateForm = new QWidget(Box);
    auto* Form = new QFormLayout(StateForm);
    Form->setContentsMargins(0, 0, 0, 0);
    StateName  = new QLineEdit(StateForm);
    StateSpeed = new QDoubleSpinBox(StateForm);
    StateSpeed->setRange(0.0, 10.0);
    StateSpeed->setSingleStep(0.1);
    StateLoop = new QComboBox(StateForm);
    StateLoop->addItems(LOOP_NAMES);
    StateSync = new QLineEdit(StateForm);
    StateSync->setPlaceholderText(QStringLiteral("none"));
    StateSync->setToolTip(QStringLiteral("States with one sync group name keep in step: a later layer's state follows the first layer "
                                         "playing one of the group (arms swinging with the legs), and a transition between two of a "
                                         "layer's states in the group goes on from where the first was"));
    StateBlend = new QComboBox(StateForm);
    StateBlend->setToolTip(QStringLiteral("None: the first clip alone. A float parameter: a blend space -- each clip plays fully at its "
                                          "position, and between two the parameter blends them"));
    StateBlendY = new QComboBox(StateForm);
    StateBlendY->setToolTip(QStringLiteral("None: a 1D blend space. A second float parameter: a 2D one -- each clip at a point "
                                           "(X, Y), the parameters' point blending those round it (speed by direction, say)"));
    SampleTable = MakeTable(StateForm, { QStringLiteral("Clip (.nmanim)"), QStringLiteral("X"), QStringLiteral("Y") });
    SampleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    SampleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    QPushButton *AddSample = nullptr, *RemoveSample = nullptr;
    auto* SampleColumn = new QVBoxLayout();
    SampleColumn->addWidget(SampleTable);
    SampleColumn->addLayout(AddRemoveRow(StateForm, QStringLiteral("clip"), AddSample, RemoveSample));
    Form->addRow(QStringLiteral("Name"), StateName);
    Form->addRow(QStringLiteral("Speed"), StateSpeed);
    Form->addRow(QStringLiteral("Loop"), StateLoop);
    Form->addRow(QStringLiteral("Sync Group"), StateSync);
    Form->addRow(QStringLiteral("Blend Parameter"), StateBlend);
    Form->addRow(QStringLiteral("Blend Parameter Y"), StateBlendY);
    Form->addRow(QStringLiteral("Clips"), SampleColumn);
    Row->addWidget(StateForm, 1);

    connect(StateList, &QListWidget::currentRowChanged, this, [this](int) { if(!bFilling) { FillState(); } });
    connect(StateName, &QLineEdit::editingFinished, this, [this]()
    {
        if(RenderCore::AnimStateNode* State = CurrentState()) { State->Name = StateName->text().trimmed().toStdString(); Edited(true); }
    });
    connect(StateSpeed, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        if(RenderCore::AnimStateNode* State = CurrentState(); State && !bFilling) { State->Speed = float(InValue); Edited(); }
    });
    connect(StateSync, &QLineEdit::editingFinished, this, [this]()
    {
        RenderCore::AnimStateNode* State = CurrentState();
        const std::string Group = StateSync->text().trimmed().toStdString();
        if(State && !bFilling && State->SyncGroup != Group) { State->SyncGroup = Group; Edited(); }
    });
    connect(StateLoop, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimStateNode* State = CurrentState()) { State->Loop = RenderCore::EAnimLoop(InIndex); Edited(); }
    });
    connect(StateBlend, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimStateNode* State = CurrentState())
        {
            State->BlendParameter = InIndex <= 0 ? std::string() : StateBlend->itemText(InIndex).toStdString();
            Edited();
            FillState();   // Y needs an X
        }
    });
    connect(StateBlendY, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimStateNode* State = CurrentState())
        {
            State->BlendParameterY = InIndex <= 0 ? std::string() : StateBlendY->itemText(InIndex).toStdString();
            Edited();
            FillState();
        }
    });
    connect(SampleTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* InItem)
    {
        RenderCore::AnimStateNode* State = CurrentState();
        if(bFilling || !State || InItem->row() < 0 || size_t(InItem->row()) >= State->Samples.size() || InItem->column() < 1) { return; }
        RenderCore::AnimBlendSample& Sample = State->Samples[size_t(InItem->row())];
        (InItem->column() == 1 ? Sample.Position : Sample.PositionY) = InItem->text().toFloat();
        Edited();
    });
    connect(AddSample, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimStateNode* State = CurrentState();
        if(!State) { return; }
        /* 1D: past the last. 2D: where the last was -- moved from there. */
        const bool  b2D  = !State->BlendParameterY.empty();
        const float Next = State->Samples.empty() ? 0.0f : State->Samples.back().Position + (b2D ? 0.0f : 100.0f);
        State->Samples.push_back({ std::string(), Next, State->Samples.empty() ? 0.0f : State->Samples.back().PositionY });
        Edited();
        FillState();
    });
    connect(RemoveSample, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimStateNode* State = CurrentState();
        const int Row = SampleTable->currentRow();
        if(!State || Row < 0 || size_t(Row) >= State->Samples.size()) { return; }
        State->Samples.erase(State->Samples.begin() + Row);
        Edited();
        FillState();
    });
    connect(Add, &QPushButton::clicked, this, [this]()
    {
        /* Right of the right-most state. */
        RenderCore::AnimLayer* Layer = CurrentLayer();
        if(!Layer) { return; }
        float Right = 0.0f;
        for(const RenderCore::AnimStateNode& Each : Layer->States) { Right = std::max(Right, Each.EditorX + 240.0f); }
        AddState(QPointF(Right, 0.0));
    });
    connect(Remove, &QPushButton::clicked, this, [this]() { RemoveState(StateList->currentRow()); });
    return Box;
}

void AnimStateEditorWindow::AddState(const QPointF& InAt)
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    if(!Layer || bReadOnly) { return; }
    RenderCore::AnimStateNode State;
    State.Name    = "State " + std::to_string(Layer->States.size() + 1);
    State.EditorX = float(InAt.x());
    State.EditorY = float(InAt.y());
    Layer->States.push_back(State);
    Edited(true);
    ShowDetail(false);
    StateList->setCurrentRow(StateList->count() - 1);
    SyncGraph();
}

void AnimStateEditorWindow::RemoveState(const int InState)
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    if(!Layer || bReadOnly || InState < 0 || size_t(InState) >= Layer->States.size()) { return; }
    if(Layer->States.size() == 1)
    {
        QMessageBox::information(this, QStringLiteral("Anim State"), QStringLiteral("A layer keeps at least one state."));
        return;
    }
    /* Its transitions go with it; the rest are renumbered. */
    Layer->States.erase(Layer->States.begin() + InState);
    Layer->Transitions.erase(std::remove_if(Layer->Transitions.begin(), Layer->Transitions.end(),
                                            [InState](const RenderCore::AnimTransition& InT) { return InT.From == InState || InT.To == InState; }),
                             Layer->Transitions.end());
    for(RenderCore::AnimTransition& Transition : Layer->Transitions)
    {
        if(Transition.From > InState) { --Transition.From; }
        if(Transition.To > InState)   { --Transition.To; }
    }
    if(Layer->EntryState >= InState && Layer->EntryState > 0) { --Layer->EntryState; }
    Edited(true);
}

void AnimStateEditorWindow::AddTransition(const int InFrom, const int InTo)
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    if(!Layer || bReadOnly || InTo < 0 || size_t(InTo) >= Layer->States.size()) { return; }
    RenderCore::AnimTransition Transition;
    Transition.From = InFrom;
    Transition.To   = InTo;
    Layer->Transitions.push_back(Transition);
    Edited(true);
    ShowDetail(true);
    TransitionList->setCurrentRow(TransitionList->count() - 1);
    SyncGraph();
}

void AnimStateEditorWindow::RemoveTransition(const int InTransition)
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    if(!Layer || bReadOnly || InTransition < 0 || size_t(InTransition) >= Layer->Transitions.size()) { return; }
    Layer->Transitions.erase(Layer->Transitions.begin() + InTransition);
    Edited(true);
}

void AnimStateEditorWindow::ShowDetail(const bool bInTransition)
{
    StateBox->setVisible(!bInTransition);
    TransitionBox->setVisible(bInTransition);
}

void AnimStateEditorWindow::SyncGraph()
{
    if(!Graph) { return; }
    RenderCore::AnimLayer* Layer = CurrentLayer();
    if(Layer && !Layer->bEditorPlaced)
    {
        AnimStateGraph::LayOut(*Layer);   // where it is drawn: not an edit to save
    }
    Graph->SetLayer(Layer, bReadOnly);
    if(TransitionBox->isVisible()) { Graph->SetSelection(-1, TransitionList->currentRow()); }
    else                           { Graph->SetSelection(StateList->currentRow(), -1); }
    UpdateLive();
}

QWidget* AnimStateEditorWindow::BuildTransitions()
{
    auto* Box = new QGroupBox(QStringLiteral("Transition -- picked on the graph; of a state's, the first that holds fires"), this);
    TransitionBox = Box;
    Box->hide();   // until one is picked
    auto* Row = new QHBoxLayout(Box);

    auto* ListColumn = new QVBoxLayout();
    TransitionList = new QListWidget(Box);
    TransitionList->setMaximumWidth(240);
    ListColumn->addWidget(TransitionList);
    QPushButton *Add = nullptr, *Remove = nullptr;
    ListColumn->addLayout(AddRemoveRow(Box, QStringLiteral("transition"), Add, Remove));
    Row->addLayout(ListColumn);
    for(QWidget* Each : std::initializer_list<QWidget*>{ TransitionList, Add, Remove }) { Each->hide(); }

    TransitionForm = new QWidget(Box);
    auto* Form = new QFormLayout(TransitionForm);
    Form->setContentsMargins(0, 0, 0, 0);
    TransitionFrom = new QComboBox(TransitionForm);
    TransitionTo   = new QComboBox(TransitionForm);
    TransitionDuration = new QDoubleSpinBox(TransitionForm);
    TransitionDuration->setRange(0.0, 10.0);
    TransitionDuration->setSingleStep(0.05);
    TransitionDuration->setSuffix(QStringLiteral(" s"));
    TransitionDuration->setToolTip(QStringLiteral("The crossfade; 0 cuts"));
    TransitionAtEnd = new QCheckBox(QStringLiteral("Only as the clip ends"), TransitionForm);
    TransitionAtEnd->setToolTip(QStringLiteral("Waits for the state's clip to come within the crossfade of its end. With no "
                                               "conditions, it then always fires: a one-shot going back"));
    ConditionTable = MakeTable(TransitionForm, { QStringLiteral("Parameter"), QStringLiteral("Test"), QStringLiteral("Value") });
    QPushButton *AddCondition = nullptr, *RemoveCondition = nullptr;
    auto* ConditionColumn = new QVBoxLayout();
    ConditionColumn->addWidget(ConditionTable);
    ConditionColumn->addLayout(AddRemoveRow(TransitionForm, QStringLiteral("condition"), AddCondition, RemoveCondition));
    Form->addRow(QStringLiteral("From"), TransitionFrom);
    Form->addRow(QStringLiteral("To"), TransitionTo);
    Form->addRow(QStringLiteral("Crossfade"), TransitionDuration);
    Form->addRow(QString(), TransitionAtEnd);
    Form->addRow(QStringLiteral("When all of"), ConditionColumn);
    Row->addWidget(TransitionForm, 1);

    connect(TransitionList, &QListWidget::currentRowChanged, this, [this](int) { if(!bFilling) { FillTransition(); } });
    connect(TransitionFrom, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimTransition* Transition = CurrentTransition()) { Transition->From = InIndex - 1; Edited(true); }
    });
    connect(TransitionTo, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(RenderCore::AnimTransition* Transition = CurrentTransition()) { Transition->To = InIndex; Edited(true); }
    });
    connect(TransitionDuration, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
    {
        if(RenderCore::AnimTransition* Transition = CurrentTransition(); Transition && !bFilling) { Transition->Duration = float(InValue); Edited(); }
    });
    connect(TransitionAtEnd, &QCheckBox::toggled, this, [this](const bool bInAtEnd)
    {
        if(RenderCore::AnimTransition* Transition = CurrentTransition(); Transition && !bFilling) { Transition->bAtEnd = bInAtEnd; Edited(true); }
    });
    connect(ConditionTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* InItem)
    {
        RenderCore::AnimTransition* Transition = CurrentTransition();
        if(bFilling || !Transition || InItem->row() < 0 || size_t(InItem->row()) >= Transition->Conditions.size() || InItem->column() != 2) { return; }
        Transition->Conditions[size_t(InItem->row())].Value = InItem->text().toFloat();
        Edited(true);
    });
    connect(AddCondition, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimTransition* Transition = CurrentTransition();
        if(!Transition) { return; }
        RenderCore::AnimCondition Condition;
        if(!Edit.Parameters.empty())
        {
            Condition.Parameter = Edit.Parameters.front().Name;
            Condition.Compare   = Edit.Parameters.front().Type == RenderCore::EAnimParameterType::Trigger ? RenderCore::EAnimCompare::Triggered
                                : Edit.Parameters.front().Type == RenderCore::EAnimParameterType::Bool    ? RenderCore::EAnimCompare::IsTrue
                                                                                                         : RenderCore::EAnimCompare::Greater;
        }
        Transition->Conditions.push_back(Condition);
        Edited(true);
        FillTransition();
    });
    connect(RemoveCondition, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimTransition* Transition = CurrentTransition();
        const int Row = ConditionTable->currentRow();
        if(!Transition || Row < 0 || size_t(Row) >= Transition->Conditions.size()) { return; }
        Transition->Conditions.erase(Transition->Conditions.begin() + Row);
        Edited(true);
        FillTransition();
    });
    connect(Add, &QPushButton::clicked, this, [this]()
    {
        RenderCore::AnimLayer* Layer = CurrentLayer();
        if(!Layer) { return; }
        const int From = std::max(StateList->currentRow(), 0);
        AddTransition(From, Layer->States.size() > 1 ? (From + 1) % int(Layer->States.size()) : 0);
    });
    connect(Remove, &QPushButton::clicked, this, [this]() { RemoveTransition(TransitionList->currentRow()); });
    return Box;
}

QWidget* AnimStateEditorWindow::BuildPreview()
{
    auto* Side   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Side);

    auto* MeshRow = new QHBoxLayout();
    MeshLabel = new QLabel(Side);
    MeshLabel->setWordWrap(true);
    auto* ChooseMesh = new QPushButton(QStringLiteral("Mesh..."), Side);
    ChooseMesh->setToolTip(QStringLiteral("Preview on another skinned mesh of the project"));
    connect(ChooseMesh, &QPushButton::clicked, this, [this]()
    {
        const QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Preview Mesh"), EditorGraphs::GetProjectRoot(),
                                                          QStringLiteral("Meshes (*.nmmesh)"));
        if(!File.isEmpty()) { PickMesh(File); }
    });
    MeshRow->addWidget(MeshLabel, 1);
    MeshRow->addWidget(ChooseMesh);
    Layout->addLayout(MeshRow);

    Preview = new AnimPreviewWidget(Side);
    Layout->addWidget(Preview, 1);

    LiveStates = new QLabel(Side);
    LiveStates->setWordWrap(true);
    Layout->addWidget(LiveStates);

    LiveBox  = new QGroupBox(QStringLiteral("Drive it -- the preview only"), Side);
    LiveForm = new QFormLayout(LiveBox);
    Layout->addWidget(LiveBox);

    Problems = new QLabel(Side);
    Problems->setWordWrap(true);
    Layout->addWidget(Problems);

    connect(Preview, &AnimPreviewWidget::Advanced, this, &AnimStateEditorWindow::UpdateLive);
    return Side;
}

// === Editing ==============================================================================

RenderCore::AnimLayer* AnimStateEditorWindow::CurrentLayer()
{
    const int Row = LayerList ? LayerList->currentRow() : -1;
    return Row >= 0 && size_t(Row) < Edit.Layers.size() ? &Edit.Layers[size_t(Row)] : nullptr;
}

RenderCore::AnimStateNode* AnimStateEditorWindow::CurrentState()
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    const int Row = StateList ? StateList->currentRow() : -1;
    return Layer && Row >= 0 && size_t(Row) < Layer->States.size() ? &Layer->States[size_t(Row)] : nullptr;
}

RenderCore::AnimTransition* AnimStateEditorWindow::CurrentTransition()
{
    RenderCore::AnimLayer* Layer = CurrentLayer();
    const int Row = TransitionList ? TransitionList->currentRow() : -1;
    return Layer && Row >= 0 && size_t(Row) < Layer->Transitions.size() ? &Layer->Transitions[size_t(Row)] : nullptr;
}

QStringList AnimStateEditorWindow::ParameterNames(const bool bInFloatsOnly) const
{
    QStringList Names;
    for(const RenderCore::AnimStateParameter& Parameter : Edit.Parameters)
    {
        if(!bInFloatsOnly || Parameter.Type == RenderCore::EAnimParameterType::Float) { Names << ToQ(Parameter.Name); }
    }
    return Names;
}

QString AnimStateEditorWindow::DescribeTransition(const RenderCore::AnimLayer& InLayer, const RenderCore::AnimTransition& InTransition) const
{
    const auto StateName = [&InLayer](const int32_t InIndex)
    {
        return InIndex < 0 ? QStringLiteral("Any") : InIndex < int32_t(InLayer.States.size()) ? ToQ(InLayer.States[size_t(InIndex)].Name) : QStringLiteral("?");
    };
    QString Text = QStringLiteral("%1 -> %2").arg(StateName(InTransition.From), StateName(InTransition.To));
    if(InTransition.bAtEnd) { Text += QStringLiteral(" (at end)"); }
    if(!InTransition.Conditions.empty()) { Text += QStringLiteral(" if %1").arg(ToQ(InTransition.Conditions.front().Parameter)); }
    if(InTransition.Conditions.size() > 1) { Text += QStringLiteral(" +%1").arg(InTransition.Conditions.size() - 1); }
    return Text;
}

void AnimStateEditorWindow::RenameParameter(const std::string& InOld, const std::string& InNew)
{
    for(RenderCore::AnimStateParameter& Parameter : Edit.Parameters)
    {
        if(Parameter.Name == InOld) { Parameter.Name = InNew; }
    }
    if(Edit.LookAt.Parameter == InOld)       { Edit.LookAt.Parameter = InNew; }
    if(Edit.LookAt.WeightParameter == InOld) { Edit.LookAt.WeightParameter = InNew; }
    for(RenderCore::AnimLayer& Layer : Edit.Layers)
    {
        if(Layer.WeightParameter == InOld) { Layer.WeightParameter = InNew; }
        for(RenderCore::AnimStateNode& State : Layer.States)
        {
            if(State.BlendParameter == InOld) { State.BlendParameter = InNew; }
            if(State.BlendParameterY == InOld) { State.BlendParameterY = InNew; }
        }
        for(RenderCore::AnimTransition& Transition : Layer.Transitions)
        {
            for(RenderCore::AnimCondition& Condition : Transition.Conditions)
            {
                if(Condition.Parameter == InOld) { Condition.Parameter = InNew; }
            }
        }
    }
}

QWidget* AnimStateEditorWindow::BuildLookAt()
{
    auto* Box = new QGroupBox(QStringLiteral("Look-At -- a chain of bones turned by an angle, after the layers"), this);
    auto* Form = new QFormLayout(Box);
    LookParameter = new QComboBox(Box);
    LookParameter->setToolTip(QStringLiteral("A float parameter: the angle, in degrees -- the camera's pitch, say (animation:SetFloat). "
                                             "None: no look-at"));
    LookWeightParameter = new QComboBox(Box);
    LookWeightParameter->setToolTip(QStringLiteral("Scales the angle (clamped 0..1): Armed, to look only with a weapon up. None: always"));
    const auto MakeAngle = [Box](const double InMin, const double InMax)
    {
        auto* Spin = new QDoubleSpinBox(Box);
        Spin->setRange(InMin, InMax);
        Spin->setSingleStep(5.0);
        Spin->setSuffix(QStringLiteral(" deg"));
        return Spin;
    };
    LookMin = MakeAngle(-180.0, 180.0);
    LookMax = MakeAngle(-180.0, 180.0);
    LookMin->setToolTip(QStringLiteral("The parameter is clamped to Min..Max"));
    LookMax->setToolTip(LookMin->toolTip());
    auto* AxisRow = new QHBoxLayout();
    for(int Index = 0; Index < 3; ++Index)
    {
        LookAxis[Index] = new QDoubleSpinBox(Box);
        LookAxis[Index]->setRange(-1.0, 1.0);
        LookAxis[Index]->setSingleStep(0.1);
        LookAxis[Index]->setDecimals(3);
        LookAxis[Index]->setPrefix(QString(QLatin1Char("XYZ"[Index])) + QStringLiteral(" "));
        LookAxis[Index]->setToolTip(QStringLiteral("What it turns about, in model space; a positive angle turns right-handed about it. "
                                                   "Pitch on a character imported facing -Z: X 1 -- or X -1 if positive should look down"));
        AxisRow->addWidget(LookAxis[Index]);
    }
    LookResponse = new QDoubleSpinBox(Box);
    LookResponse->setRange(0.0, 100.0);
    LookResponse->setSingleStep(1.0);
    LookResponse->setSuffix(QStringLiteral(" /s"));
    LookResponse->setToolTip(QStringLiteral("How fast it follows the parameter: higher is snappier. 0: at once"));
    LookBones = new QLineEdit(Box);
    LookBones->setPlaceholderText(QStringLiteral("spine_01:0.3, spine_02:0.3, neck:0.4"));
    LookBones->setToolTip(QStringLiteral("The chain, comma-separated, each bone:share -- its share of the angle. The shares usually "
                                         "add to 1; turns add up along the chain, so the last bone ends at the whole angle"));
    Form->addRow(QStringLiteral("Angle Parameter"), LookParameter);
    Form->addRow(QStringLiteral("Weight Parameter"), LookWeightParameter);
    Form->addRow(QStringLiteral("Min"), LookMin);
    Form->addRow(QStringLiteral("Max"), LookMax);
    Form->addRow(QStringLiteral("Axis"), AxisRow);
    Form->addRow(QStringLiteral("Response"), LookResponse);
    Form->addRow(QStringLiteral("Bones"), LookBones);

    const auto PickedName = [](QComboBox* InCombo, const int InIndex)
    {
        return InIndex <= 0 ? std::string() : InCombo->itemText(InIndex).toStdString();
    };
    connect(LookParameter, QOverload<int>::of(&QComboBox::activated), this, [this, PickedName](const int InIndex)
    {
        Edit.LookAt.Parameter = PickedName(LookParameter, InIndex);
        Edited();
        FillLookAt();
    });
    connect(LookWeightParameter, QOverload<int>::of(&QComboBox::activated), this, [this, PickedName](const int InIndex)
    {
        Edit.LookAt.WeightParameter = PickedName(LookWeightParameter, InIndex);
        Edited();
    });
    const auto OnSpin = [this](QDoubleSpinBox* InSpin, float& OutValue)
    {
        connect(InSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, &OutValue](const double InValue)
        {
            if(!bFilling) { OutValue = float(InValue); Edited(); }
        });
    };
    OnSpin(LookMin, Edit.LookAt.MinAngle);
    OnSpin(LookMax, Edit.LookAt.MaxAngle);
    OnSpin(LookResponse, Edit.LookAt.Response);
    for(int Index = 0; Index < 3; ++Index) { OnSpin(LookAxis[Index], Edit.LookAt.Axis[Index]); }
    connect(LookBones, &QLineEdit::editingFinished, this, [this]()
    {
        std::vector<RenderCore::AnimLookBone> Bones;
        for(const QString& Each : LookBones->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
        {
            const QStringList Parts = Each.split(QLatin1Char(':'));
            RenderCore::AnimLookBone Bone;
            Bone.Bone  = Parts[0].trimmed().toStdString();
            Bone.Share = Parts.size() > 1 ? Parts[1].trimmed().toFloat() : 1.0f;
            if(!Bone.Bone.empty()) { Bones.push_back(Bone); }
        }
        if(Bones.size() == Edit.LookAt.Bones.size()
           && std::equal(Bones.begin(), Bones.end(), Edit.LookAt.Bones.begin(),
                         [](const RenderCore::AnimLookBone& InA, const RenderCore::AnimLookBone& InB) { return InA.Bone == InB.Bone && InA.Share == InB.Share; }))
        {
            return;
        }
        Edit.LookAt.Bones = std::move(Bones);
        Edited();
        FillLookAt();
    });
    return Box;
}

void AnimStateEditorWindow::Edited(const bool bInStructure, const bool bInDirty)
{
    bDirty = bInDirty;
    if(bInStructure)
    {
        FillAll();   // and the graph, through FillLayer
    }
    else
    {
        SyncGraph();   // a state's blend or clip: its box says so
    }

    /* What would keep it from playing, and what would quietly do nothing. */
    QStringList Found;
    if(const char* Defect = Edit.FindDefect()) { Found << QString::fromLatin1(Defect); }
    if(Edit.Layers.empty()) { Found << QStringLiteral("No layers: it plays nothing"); }
    const auto CheckParameter = [&](const std::string& InName, const QString& InWhere)
    {
        if(!InName.empty() && Edit.FindParameter(InName) < 0) { Found << QStringLiteral("%1 names a parameter it lacks: %2").arg(InWhere, ToQ(InName)); }
    };
    const RenderCore::SkeletonData* Skeleton = Preview ? Preview->GetSkeleton() : nullptr;
    for(const RenderCore::AnimLayer& Layer : Edit.Layers)
    {
        const QString LayerName = ToQ(Layer.Name);
        CheckParameter(Layer.WeightParameter, QStringLiteral("Layer %1").arg(LayerName));
        for(const std::string& Bone : Layer.MaskBones)
        {
            if(Skeleton && Skeleton->FindBone(Bone) < 0) { Found << QStringLiteral("Layer %1 masks a bone the skeleton lacks: %2").arg(LayerName, ToQ(Bone)); }
        }
        for(const RenderCore::AnimStateNode& State : Layer.States)
        {
            const QString Where = QStringLiteral("State %1").arg(ToQ(State.Name));
            CheckParameter(State.BlendParameter, Where);
            CheckParameter(State.BlendParameterY, Where);
            if(!State.BlendParameterY.empty() && State.BlendParameter.empty())
            {
                Found << QStringLiteral("%1: a Y blend parameter but no X -- it plays its first clip alone").arg(Where);
            }
            for(const RenderCore::AnimBlendSample& Sample : State.Samples)
            {
                if(Sample.Clip.empty() || !QFileInfo::exists(EditorAnimation::FileOfRuntimePath(ToQ(Sample.Clip))))
                {
                    Found << QStringLiteral("%1: a clip that is not there: %2").arg(Where, Sample.Clip.empty() ? QStringLiteral("(none)") : ToQ(Sample.Clip));
                }
            }
        }
        for(const RenderCore::AnimTransition& Transition : Layer.Transitions)
        {
            for(const RenderCore::AnimCondition& Condition : Transition.Conditions)
            {
                CheckParameter(Condition.Parameter, QStringLiteral("Transition %1").arg(DescribeTransition(Layer, Transition)));
            }
        }
    }
    const RenderCore::AnimLookAt& Look = Edit.LookAt;
    CheckParameter(Look.Parameter, QStringLiteral("The look-at"));
    CheckParameter(Look.WeightParameter, QStringLiteral("The look-at"));
    if(!Look.Parameter.empty() && Look.Bones.empty()) { Found << QStringLiteral("The look-at has an angle parameter but no bones: it turns nothing"); }
    if(Look.Parameter.empty() && !Look.Bones.empty()) { Found << QStringLiteral("The look-at has bones but no angle parameter: it turns nothing"); }
    for(const RenderCore::AnimLookBone& Bone : Look.Bones)
    {
        if(Skeleton && Skeleton->FindBone(Bone.Bone) < 0) { Found << QStringLiteral("The look-at turns a bone the skeleton lacks: %1").arg(ToQ(Bone.Bone)); }
    }
    QStringList Escaped;
    for(const QString& Each : Found) { Escaped << Each.toHtmlEscaped(); }
    Problems->setText(Found.isEmpty() ? QString() : QStringLiteral("<b>Problems</b><br>") + Escaped.join(QStringLiteral("<br>")));

    RestartPreview();
    UpdateTitle();
}

void AnimStateEditorWindow::RestartPreview()
{
    if(!Preview || !Preview->HasSkeleton()) { UpdateLive(); return; }
    if(Edit.FindDefect())
    {
        Preview->PlayState(nullptr);
        UpdateLive();
        return;
    }
    /* What the drive controls say survives a restart. */
    std::map<std::string, float> Live;
    for(const RenderCore::AnimStateParameter& Parameter : Edit.Parameters)
    {
        Live[Parameter.Name] = Preview->GetInstance().GetParameter(Parameter.Name, Parameter.Default);
    }
    Preview->PlayState(std::make_shared<RenderCore::AnimStateData>(Edit));
    for(const auto& Each : Live)
    {
        const int32_t Index = Edit.FindParameter(Each.first);
        if(Index >= 0 && Edit.Parameters[size_t(Index)].Type != RenderCore::EAnimParameterType::Trigger)
        {
            Preview->GetInstance().SetParameter(Each.first, Each.second);
        }
    }
    UpdateLive();
}

void AnimStateEditorWindow::PickMesh(const QString& InFile)
{
    PreviewMesh = RenderCore::MeshData();
    MeshRuntime.clear();
    if(!InFile.isEmpty())
    {
        EngineCore::Archive Ar = EditorAnimation::ReadAssetArchive(InFile);
        RenderCore::MeshData Loaded;
        if(RenderCore::SerializeMesh(Ar, Loaded) && Loaded.IsSkinned())
        {
            PreviewMesh = std::move(Loaded);
            MeshRuntime = EditorContent::ToRuntimePath(InFile);
        }
        else
        {
            QMessageBox::information(this, QStringLiteral("Preview Mesh"), QStringLiteral("%1 is not a skinned mesh.").arg(QFileInfo(InFile).fileName()));
        }
    }
    if(MeshRuntime.isEmpty())
    {
        MeshLabel->setText(QStringLiteral("No skinned mesh on its clips' skeleton was found: choose one to preview on."));
        return;
    }
    MeshLabel->setText(QStringLiteral("Previewed on <b>%1</b>").arg(MeshRuntime.toHtmlEscaped()));
    Preview->SetMesh(MeshRuntime, PreviewMesh);

    /* Mask bones complete from its skeleton. */
    if(const RenderCore::SkeletonData* Skeleton = Preview->GetSkeleton())
    {
        QStringList Bones;
        for(const RenderCore::SkeletonBone& Bone : Skeleton->Bones) { Bones << ToQ(Bone.Name); }
        auto* Completer = new QCompleter(Bones, LayerMask);
        Completer->setCaseSensitivity(Qt::CaseInsensitive);
        Completer->setFilterMode(Qt::MatchContains);
        LayerMask->setCompleter(Completer);
    }
    RestartPreview();
}

// === Filling ==============================================================================

void AnimStateEditorWindow::FillAll()
{
    FillParameters();
    FillLayers();
    FillLookAt();
    FillLive();
}

void AnimStateEditorWindow::FillLookAt()
{
    const bool bWas = bFilling;
    bFilling = true;
    const RenderCore::AnimLookAt& Look = Edit.LookAt;
    const auto FillCombo = [this](QComboBox* InCombo, const QStringList& InNames, const std::string& InPicked)
    {
        InCombo->clear();
        InCombo->addItem(QStringLiteral("(none)"));
        InCombo->addItems(InNames);
        InCombo->setCurrentIndex(std::max(InCombo->findText(ToQ(InPicked)), 0));
    };
    FillCombo(LookParameter, ParameterNames(true), Look.Parameter);
    QStringList Weights;
    for(const RenderCore::AnimStateParameter& Parameter : Edit.Parameters)
    {
        if(Parameter.Type != RenderCore::EAnimParameterType::Trigger) { Weights << ToQ(Parameter.Name); }
    }
    FillCombo(LookWeightParameter, Weights, Look.WeightParameter);
    LookMin->setValue(double(Look.MinAngle));
    LookMax->setValue(double(Look.MaxAngle));
    LookResponse->setValue(double(Look.Response));
    for(int Index = 0; Index < 3; ++Index) { LookAxis[Index]->setValue(double(Look.Axis[Index])); }
    QStringList Bones;
    for(const RenderCore::AnimLookBone& Bone : Look.Bones) { Bones << QStringLiteral("%1:%2").arg(ToQ(Bone.Bone)).arg(double(Bone.Share)); }
    LookBones->setText(Bones.join(QStringLiteral(", ")));

    /* The rest only means something with an angle to follow. */
    LookParameter->setEnabled(!bReadOnly);
    const bool bOn = !Look.Parameter.empty() && !bReadOnly;
    for(QWidget* Each : std::initializer_list<QWidget*>{ LookWeightParameter, LookMin, LookMax, LookAxis[0], LookAxis[1], LookAxis[2], LookResponse, LookBones })
    {
        Each->setEnabled(bOn);
    }
    bFilling = bWas;
}

void AnimStateEditorWindow::FillParameters()
{
    const bool bWas = bFilling;
    bFilling = true;
    const int Row = ParameterTable->currentRow();
    ParameterTable->setRowCount(int(Edit.Parameters.size()));
    for(size_t Index = 0; Index < Edit.Parameters.size(); ++Index)
    {
        const RenderCore::AnimStateParameter& Parameter = Edit.Parameters[Index];
        ParameterTable->setItem(int(Index), 0, new QTableWidgetItem(ToQ(Parameter.Name)));
        auto* Type = new QComboBox(ParameterTable);
        Type->addItems(TYPE_NAMES);
        Type->setCurrentIndex(int(Parameter.Type));
        connect(Type, QOverload<int>::of(&QComboBox::activated), this, [this, Index](const int InType)
        {
            if(Index < Edit.Parameters.size()) { Edit.Parameters[Index].Type = RenderCore::EAnimParameterType(InType); Edited(true); }
        });
        ParameterTable->setCellWidget(int(Index), 1, Type);
        ParameterTable->setItem(int(Index), 2, new QTableWidgetItem(QString::number(double(Parameter.Default))));
    }
    if(Row >= 0 && Row < ParameterTable->rowCount()) { ParameterTable->setCurrentCell(Row, 0); }
    bFilling = bWas;
}

void AnimStateEditorWindow::FillLayers()
{
    const bool bWas = bFilling;
    bFilling = true;
    const int Row = std::max(LayerList->currentRow(), 0);
    LayerList->clear();
    for(const RenderCore::AnimLayer& Layer : Edit.Layers)
    {
        LayerList->addItem(ToQ(Layer.Name.empty() ? std::string("(unnamed)") : Layer.Name));
    }
    LayerList->setCurrentRow(std::min(Row, LayerList->count() - 1));
    bFilling = bWas;
    FillLayer();
}

void AnimStateEditorWindow::FillLayer()
{
    const bool bWas = bFilling;
    bFilling = true;
    RenderCore::AnimLayer* Layer = CurrentLayer();
    for(QWidget* Each : std::initializer_list<QWidget*>{ LayerName, LayerBlend, LayerBase, LayerWeight, LayerWeightParameter, LayerMask, LayerEntry })
    {
        Each->setEnabled(Layer != nullptr && !bReadOnly);
    }
    LayerWeightParameter->clear();
    LayerWeightParameter->addItem(QStringLiteral("(none)"));
    LayerWeightParameter->addItems(ParameterNames(true));
    LayerEntry->clear();
    if(Layer)
    {
        LayerName->setText(ToQ(Layer->Name));
        LayerWeight->setValue(double(Layer->Weight));
        LayerBlend->setCurrentIndex(int(Layer->Blend));
        LayerBase->setCurrentIndex(int(Layer->AdditiveBase));
        LayerBase->setEnabled(Layer->Blend == RenderCore::EAnimLayerBlend::Additive && !bReadOnly);
        LayerWeightParameter->setCurrentIndex(std::max(LayerWeightParameter->findText(ToQ(Layer->WeightParameter)), 0));
        QStringList Mask;
        for(const std::string& Bone : Layer->MaskBones) { Mask << ToQ(Bone); }
        LayerMask->setText(Mask.join(QStringLiteral(", ")));
        for(const RenderCore::AnimStateNode& State : Layer->States) { LayerEntry->addItem(ToQ(State.Name)); }
        LayerEntry->setCurrentIndex(Layer->EntryState);
    }
    bFilling = bWas;
    FillStates();
    FillTransitions();
    SyncGraph();
}

void AnimStateEditorWindow::FillStates()
{
    const bool bWas = bFilling;
    bFilling = true;
    const int Row = std::max(StateList->currentRow(), 0);
    StateList->clear();
    if(const RenderCore::AnimLayer* Layer = CurrentLayer())
    {
        for(size_t Index = 0; Index < Layer->States.size(); ++Index)
        {
            const RenderCore::AnimStateNode& State = Layer->States[Index];
            StateList->addItem(ToQ(State.Name) + (int32_t(Index) == Layer->EntryState ? QStringLiteral("  (entry)") : QString()));
        }
        StateList->setCurrentRow(std::min(Row, StateList->count() - 1));
    }
    bFilling = bWas;
    FillState();
}

void AnimStateEditorWindow::FillState()
{
    const bool bWas = bFilling;
    bFilling = true;
    RenderCore::AnimStateNode* State = CurrentState();
    StateForm->setEnabled(State != nullptr && !bReadOnly);
    StateBlend->clear();
    StateBlend->addItem(QStringLiteral("(none)"));
    StateBlend->addItems(ParameterNames(true));
    StateBlendY->clear();
    StateBlendY->addItem(QStringLiteral("(none)"));
    StateBlendY->addItems(ParameterNames(true));
    SampleTable->setRowCount(0);
    if(State)
    {
        StateName->setText(ToQ(State->Name));
        StateSpeed->setValue(double(State->Speed));
        StateLoop->setCurrentIndex(int(State->Loop));
        StateSync->setText(ToQ(State->SyncGroup));
        StateBlend->setCurrentIndex(std::max(StateBlend->findText(ToQ(State->BlendParameter)), 0));
        StateBlendY->setCurrentIndex(std::max(StateBlendY->findText(ToQ(State->BlendParameterY)), 0));
        StateBlendY->setEnabled(!State->BlendParameter.empty() || !State->BlendParameterY.empty());
        SampleTable->setColumnHidden(2, State->BlendParameterY.empty());

        /* Clips by file name -- what tells them apart in a narrow column --
         * with the runtime path as the data and the tip. A name two folders
         * share gets its folder. */
        QStringList Clips;
        QMap<QString, int> NameCount;
        for(const QString& Each : EditorContent::ListAnimationPaths())
        {
            if(!Each.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive)) { continue; }
            Clips << Each;
            ++NameCount[QFileInfo(Each).fileName()];
        }
        const auto Label = [&NameCount](const QString& InPath)
        {
            const QString Name = QFileInfo(InPath).fileName();
            return NameCount.value(Name) > 1 ? QStringLiteral("%1 (%2)").arg(Name, QFileInfo(InPath).path()) : Name;
        };
        SampleTable->setRowCount(int(State->Samples.size()));
        for(size_t Index = 0; Index < State->Samples.size(); ++Index)
        {
            const QString Current = ToQ(State->Samples[Index].Clip);
            auto* Clip = new QComboBox(SampleTable);
            Clip->addItem(QStringLiteral("(choose a clip)"), QString());
            for(const QString& Each : Clips)
            {
                Clip->addItem(Label(Each), Each);
                Clip->setItemData(Clip->count() - 1, Each, Qt::ToolTipRole);
            }
            int At = Clip->findData(Current);
            if(At < 0 && !Current.isEmpty())
            {
                /* A clip that is not there (moved, deleted): kept, so it can be seen and replaced. */
                Clip->addItem(QStringLiteral("%1 (missing)").arg(Current), Current);
                At = Clip->count() - 1;
            }
            Clip->setCurrentIndex(std::max(At, 0));
            Clip->setToolTip(Current.isEmpty() ? QStringLiteral("An .nmanim, by runtime path") : Current);
            connect(Clip, QOverload<int>::of(&QComboBox::activated), this, [this, Clip, Index](const int InIndex)
            {
                RenderCore::AnimStateNode* State = CurrentState();
                if(!State || Index >= State->Samples.size()) { return; }
                const std::string Picked = Clip->itemData(InIndex).toString().toStdString();
                if(Picked == State->Samples[Index].Clip) { return; }
                State->Samples[Index].Clip = Picked;
                Clip->setToolTip(QString::fromStdString(Picked));
                Edited();
                if(MeshRuntime.isEmpty()) { PickMesh(EditorAnimation::FindMeshOn(EditorAnimation::SkeletonOfAnimation(Path))); }
            });
            SampleTable->setCellWidget(int(Index), 0, Clip);
            SampleTable->setItem(int(Index), 1, new QTableWidgetItem(QString::number(double(State->Samples[Index].Position))));
            SampleTable->setItem(int(Index), 2, new QTableWidgetItem(QString::number(double(State->Samples[Index].PositionY))));
        }
    }
    bFilling = bWas;
}

void AnimStateEditorWindow::FillTransitions()
{
    const bool bWas = bFilling;
    bFilling = true;
    const int Row = std::max(TransitionList->currentRow(), 0);
    TransitionList->clear();
    if(const RenderCore::AnimLayer* Layer = CurrentLayer())
    {
        for(const RenderCore::AnimTransition& Transition : Layer->Transitions) { TransitionList->addItem(DescribeTransition(*Layer, Transition)); }
        TransitionList->setCurrentRow(std::min(Row, TransitionList->count() - 1));
    }
    bFilling = bWas;
    FillTransition();
}

void AnimStateEditorWindow::FillTransition()
{
    const bool bWas = bFilling;
    bFilling = true;
    RenderCore::AnimLayer*      Layer      = CurrentLayer();
    RenderCore::AnimTransition* Transition = CurrentTransition();
    TransitionForm->setEnabled(Transition != nullptr && !bReadOnly);
    TransitionFrom->clear();
    TransitionTo->clear();
    ConditionTable->setRowCount(0);
    if(Layer && Transition)
    {
        TransitionFrom->addItem(QStringLiteral("Any state"));
        for(const RenderCore::AnimStateNode& State : Layer->States)
        {
            TransitionFrom->addItem(ToQ(State.Name));
            TransitionTo->addItem(ToQ(State.Name));
        }
        TransitionFrom->setCurrentIndex(Transition->From + 1);
        TransitionTo->setCurrentIndex(Transition->To);
        TransitionDuration->setValue(double(Transition->Duration));
        TransitionAtEnd->setChecked(Transition->bAtEnd);

        const QStringList Names = ParameterNames(false);
        ConditionTable->setRowCount(int(Transition->Conditions.size()));
        for(size_t Index = 0; Index < Transition->Conditions.size(); ++Index)
        {
            const RenderCore::AnimCondition& Condition = Transition->Conditions[Index];
            auto* Parameter = new QComboBox(ConditionTable);
            Parameter->addItems(Names);
            if(Parameter->findText(ToQ(Condition.Parameter)) < 0) { Parameter->addItem(ToQ(Condition.Parameter)); }
            Parameter->setCurrentText(ToQ(Condition.Parameter));
            connect(Parameter, QOverload<int>::of(&QComboBox::activated), this, [this, Parameter, Index]()
            {
                RenderCore::AnimTransition* Current = CurrentTransition();
                if(!Current || Index >= Current->Conditions.size()) { return; }
                Current->Conditions[Index].Parameter = Parameter->currentText().toStdString();
                Edited(true);
            });
            auto* Compare = new QComboBox(ConditionTable);
            Compare->addItems(COMPARE_NAMES);
            Compare->setCurrentIndex(int(Condition.Compare));
            connect(Compare, QOverload<int>::of(&QComboBox::activated), this, [this, Index](const int InCompare)
            {
                RenderCore::AnimTransition* Current = CurrentTransition();
                if(!Current || Index >= Current->Conditions.size()) { return; }
                Current->Conditions[Index].Compare = RenderCore::EAnimCompare(InCompare);
                Edited(true);
            });
            ConditionTable->setCellWidget(int(Index), 0, Parameter);
            ConditionTable->setCellWidget(int(Index), 1, Compare);
            ConditionTable->setItem(int(Index), 2, new QTableWidgetItem(QString::number(double(Condition.Value))));
        }
    }
    bFilling = bWas;
}

void AnimStateEditorWindow::FillLive()
{
    while(LiveForm->rowCount() > 0) { LiveForm->removeRow(0); }
    LiveControls.clear();
    for(const RenderCore::AnimStateParameter& Parameter : Edit.Parameters)
    {
        const std::string Name  = Parameter.Name;
        const QString     Label = ToQ(Name);
        const float       Now   = Preview->GetInstance().GetParameter(Name, Parameter.Default);
        if(Parameter.Type == RenderCore::EAnimParameterType::Float)
        {
            auto* Value = new QDoubleSpinBox(LiveBox);
            Value->setRange(-100000.0, 100000.0);
            Value->setValue(double(Now));
            connect(Value, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, Name](const double InValue)
            {
                Preview->GetInstance().SetParameter(Name, float(InValue));
                if(!Preview->IsPlaying()) { Preview->Repose(); }
            });
            LiveForm->addRow(Label, Value);
            LiveControls.push_back({ Name, Parameter.Type, Value });
        }
        else if(Parameter.Type == RenderCore::EAnimParameterType::Bool)
        {
            auto* Value = new QCheckBox(LiveBox);
            Value->setChecked(Now != 0.0f);
            connect(Value, &QCheckBox::toggled, this, [this, Name](const bool bInValue)
            {
                Preview->GetInstance().SetParameter(Name, bInValue ? 1.0f : 0.0f);
                if(!Preview->IsPlaying()) { Preview->Repose(); }
            });
            LiveForm->addRow(Label, Value);
            LiveControls.push_back({ Name, Parameter.Type, Value });
        }
        else
        {
            auto* Fire = new QPushButton(QStringLiteral("Fire"), LiveBox);
            connect(Fire, &QPushButton::clicked, this, [this, Name]()
            {
                Preview->GetInstance().SetParameter(Name, 1.0f);
                if(!Preview->IsPlaying()) { Preview->SetPlaying(true); }
            });
            LiveForm->addRow(Label, Fire);
            LiveControls.push_back({ Name, Parameter.Type, Fire });
        }
    }
    LiveBox->setVisible(!Edit.Parameters.empty());
}

void AnimStateEditorWindow::UpdateLive()
{
    if(!LiveStates) { return; }
    const RenderCore::AnimInstance& Instance = Preview->GetInstance();
    SyncParameterControls(LiveControls, Instance);
    QStringList Lines;
    for(size_t Layer = 0; Layer < Instance.GetLayerCount() && Layer < Edit.Layers.size(); ++Layer)
    {
        Lines << QStringLiteral("<b>%1</b>: %2%3").arg(ToQ(Edit.Layers[Layer].Name).toHtmlEscaped(),
                                                       QString::fromUtf8(Instance.GetCurrentStateName(Layer)).toHtmlEscaped(),
                                                       Instance.IsInTransition(Layer) ? QStringLiteral(" (blending in)") : QString());
    }
    LiveStates->setText(Lines.join(QStringLiteral("<br>")));
    const int Layer = LayerList ? LayerList->currentRow() : -1;
    if(Graph && Layer >= 0 && size_t(Layer) < Instance.GetLayerCount())
    {
        Graph->SetLive(Instance.GetCurrentState(size_t(Layer)), Instance.IsInTransition(size_t(Layer)));
    }
}

void AnimStateEditorWindow::UpdateTitle()
{
    const bool bShowDirty = bDirty && !bReadOnly;
    SaveButton->setEnabled(bShowDirty && !Edit.FindDefect());
    RevertButton->setEnabled(bShowDirty);
    setWindowTitle(QStringLiteral("%1%2 - Anim State%3").arg(QFileInfo(Path).fileName(), bShowDirty ? QStringLiteral("*") : QString(),
                                                             bReadOnly ? QStringLiteral(" (engine, read-only)") : QString()));
}

bool AnimStateEditorWindow::Save()
{
    if(bReadOnly) { return true; }
    if(const char* Defect = Edit.FindDefect())
    {
        QMessageBox::warning(this, QStringLiteral("Save Anim State"), QStringLiteral("It cannot be saved as it is: %1").arg(QString::fromLatin1(Defect)));
        return false;
    }
    if(!RenderCore::SaveAnimStateFile(Path.toStdString(), Edit))
    {
        QMessageBox::warning(this, QStringLiteral("Save Anim State"), QStringLiteral("Could not write %1 -- see the log").arg(Path));
        return false;
    }
    Saved  = Edit;
    bDirty = false;
    EditorContent::StageAsset(Path);   // levels playing it load it afresh
    UpdateTitle();
    return true;
}

void AnimStateEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    if(bDirty && !bReadOnly)
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Anim State"), QStringLiteral("Save the changes to %1?").arg(QFileInfo(Path).fileName()),
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
