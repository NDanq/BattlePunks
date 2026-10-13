#include "Private/Editor/AI/BehaviorTreeEditorWindow.h"

#if WITH_EDITOR && WITH_AICORE

#include <algorithm>
#include <cmath>
#include <functional>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include "Private/Editor/AI/EditorAI.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphView.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/BehaviorTreeFile.h"

/* BehaviorTreeEditorWindow.h has the window's layout. */
namespace
{
    using Node = BehaviorTreeEditorWindow::Node;

    std::vector<BehaviorTreeEditorWindow*>& Windows()
    {
        static std::vector<BehaviorTreeEditorWindow*> Open;
        return Open;
    }

    // === The palette's defaults ============================================

    /* What a new node of a built-in type starts with: the params its code
     * reads (BTNodes.cpp), so the details pane shows what to fill in. */
    std::vector<std::pair<const char*, const char*>> DefaultParams(const std::string& InType)
    {
        if(InType == "Wait")            { return { { "Seconds", "1" } }; }
        if(InType == "SetBlackboard")   { return { { "Key", "" }, { "Value", "" } }; }
        if(InType == "ClearBlackboard") { return { { "Key", "" } }; }
        if(InType == "MoveTo")          { return { { "Key", "" }, { "AcceptanceRadius", "50" } }; }
        if(InType == "MoveToRandom")    { return { { "Radius", "1000" } }; }
        if(InType == "FindCover")       { return { { "Threat", "TargetActor" }, { "Out", "" }, { "Radius", "1500" } }; }
        if(InType == "FaceTarget")      { return { { "Key", "TargetActor" }, { "Tolerance", "10" } }; }
        if(InType == "PlayAnimParam")   { return { { "Param", "" }, { "Value", "1" } }; }
        if(InType == "RunScript")       { return { { "Method", "" } }; }
        if(InType == "Log")             { return { { "Text", "" } }; }
        if(InType == "BlackboardCondition") { return { { "Key", "" }, { "Op", "IsSet" } }; }
        if(InType == "Cooldown" || InType == "TimeLimit") { return { { "Seconds", "5" } }; }
        if(InType == "Loop")            { return { { "Count", "3" } }; }
        if(InType == "DistanceToKey")   { return { { "Key", "TargetActor" }, { "Out", "" } }; }
        if(InType == "Parallel")        { return { { "Success", "All" }, { "Failure", "One" } }; }
        if(InType == "UtilitySelector") { return { { "Interval", "0.3" }, { "Hysteresis", "0.15" } }; }
        return {};
    }

    std::vector<AICore::BTParam> MakeParams(const std::string& InType)
    {
        std::vector<AICore::BTParam> Out;
        for(const auto& [Name, Text] : DefaultParams(InType))
        {
            AICore::BTParam Param;
            Param.Name = Name;
            Param.Text = Text;
            Out.push_back(Param);
        }
        return Out;
    }

    /* A decorator's or service's params as one cell: "Key=Value; Key=Value". */
    QString ParamsText(const std::vector<AICore::BTParam>& InParams)
    {
        QStringList Parts;
        for(const AICore::BTParam& Param : InParams)
        {
            Parts << QStringLiteral("%1=%2").arg(QString::fromStdString(Param.Name), QString::fromStdString(Param.Text));
        }
        return Parts.join(QStringLiteral("; "));
    }

    std::vector<AICore::BTParam> ParseParamsText(const QString& InText)
    {
        std::vector<AICore::BTParam> Out;
        for(const QString& Part : InText.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        {
            const int Equals = Part.indexOf(QLatin1Char('='));
            AICore::BTParam Param;
            Param.Name = (Equals < 0 ? Part : Part.left(Equals)).trimmed().toStdString();
            Param.Text = Equals < 0 ? std::string() : Part.mid(Equals + 1).trimmed().toStdString();
            if(!Param.Name.empty()) { Out.push_back(Param); }
        }
        return Out;
    }

    QString FloatText(const float InValue) { return QString::number(double(InValue), 'g', 6); }

    // === The tree ===========================================================

    std::unique_ptr<Node> FromAsset(const AICore::BehaviorTreeAsset& InAsset, const uint32_t InIndex, Node* InParent)
    {
        auto Out = std::make_unique<Node>();
        Out->Def = InAsset.Nodes[InIndex];
        Out->Def.Children.clear();
        Out->Parent = InParent;
        for(const uint32_t Child : InAsset.Nodes[InIndex].Children)
        {
            if(Child < InAsset.Nodes.size() && Child > InIndex) { Out->Children.push_back(FromAsset(InAsset, Child, Out.get())); }
        }
        return Out;
    }

    /* Depth-first, a node before its children: the order a file reads in. */
    uint32_t FlattenInto(Node& InNode, AICore::BehaviorTreeAsset& OutAsset)
    {
        const uint32_t Index = uint32_t(OutAsset.Nodes.size());
        InNode.Flat = int(Index);
        OutAsset.Nodes.push_back(InNode.Def);
        OutAsset.Nodes[Index].Children.clear();
        for(const std::unique_ptr<Node>& Child : InNode.Children)
        {
            const uint32_t ChildIndex = FlattenInto(*Child, OutAsset);
            OutAsset.Nodes[Index].Children.push_back(ChildIndex);
        }
        return Index;
    }

    std::unique_ptr<Node> Clone(const Node& InNode, Node* InParent)
    {
        auto Out = std::make_unique<Node>();
        Out->Def = InNode.Def;
        Out->Parent = InParent;
        for(const std::unique_ptr<Node>& Child : InNode.Children) { Out->Children.push_back(Clone(*Child, Out.get())); }
        return Out;
    }

    Node* FindFlat(Node* InNode, const int InFlat)
    {
        if(!InNode) { return nullptr; }
        if(InNode->Flat == InFlat) { return InNode; }
        for(const std::unique_ptr<Node>& Child : InNode->Children)
        {
            if(Node* Found = FindFlat(Child.get(), InFlat)) { return Found; }
        }
        return nullptr;
    }

    bool IsComposite(const Node& InNode) { return InNode.Def.Kind == AICore::EBTNodeKind::Composite; }

    bool IsKnown(const Node& InNode)
    {
        if(!IsComposite(InNode) && !AICore::FindBTTask(InNode.Def.Type.c_str())) { return false; }
        for(const AICore::BTDecoratorDef& D : InNode.Def.Decorators) { if(!AICore::FindBTDecorator(D.Type.c_str())) { return false; } }
        for(const AICore::BTServiceDef& S : InNode.Def.Services)     { if(!AICore::FindBTService(S.Type.c_str())) { return false; } }
        return true;
    }

    QString DescribeDecorator(const AICore::BTDecoratorDef& InD)
    {
        QString Text = QStringLiteral("◇ ") + QString::fromStdString(InD.Type);
        QStringList Values;
        for(const AICore::BTParam& Param : InD.Params) { Values << QString::fromStdString(Param.Text); }
        if(!Values.isEmpty()) { Text += QLatin1Char(' ') + Values.join(QLatin1Char(' ')); }
        if(InD.Abort != AICore::EBTAbortMode::None) { Text += QStringLiteral("  [abort %1]").arg(QString::fromLatin1(AICore::GetBTAbortModeName(InD.Abort))); }
        return Text;
    }

    // === The response curve =================================================

    class CurvePreview final : public QWidget
    {
    public:
        explicit CurvePreview(QWidget* InParent) : QWidget(InParent) { setMinimumHeight(110); }
        void SetCurve(const AICore::BTConsideration* InCurve)
        {
            bHas = InCurve != nullptr;
            if(InCurve) { Curve = *InCurve; }
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter Painter(this);
            Painter.setRenderHint(QPainter::Antialiasing);
            const QRectF Area = QRectF(rect()).adjusted(28, 8, -8, -18);
            Painter.fillRect(rect(), palette().base());
            Painter.setPen(QPen(palette().mid().color(), 1));
            Painter.drawRect(Area);
            Painter.setPen(palette().text().color());
            Painter.drawText(QRectF(0, Area.top() - 6, 24, 14), Qt::AlignRight, QStringLiteral("1"));
            Painter.drawText(QRectF(0, Area.bottom() - 8, 24, 14), Qt::AlignRight, QStringLiteral("0"));
            if(!bHas)
            {
                Painter.drawText(Area, Qt::AlignCenter, QStringLiteral("Pick a consideration to see its curve"));
                return;
            }
            Painter.drawText(QRectF(Area.left(), Area.bottom() + 2, Area.width(), 14), Qt::AlignLeft,
                             QString::number(double(Curve.Min)));
            Painter.drawText(QRectF(Area.left(), Area.bottom() + 2, Area.width(), 14), Qt::AlignRight,
                             QString::number(double(Curve.Max)));
            Painter.drawText(QRectF(Area.left(), Area.bottom() + 2, Area.width(), 14), Qt::AlignHCenter,
                             QString::fromStdString(Curve.Input));
            QPainterPath Line;
            const int Steps = 96;
            for(int Step = 0; Step <= Steps; ++Step)
            {
                const float X = float(Step) / float(Steps);
                const float Y = Curve.Evaluate(X);
                const QPointF At(Area.left() + double(X) * Area.width(), Area.bottom() - double(Y) * Area.height());
                if(Step == 0) { Line.moveTo(At); } else { Line.lineTo(At); }
            }
            Painter.setPen(QPen(QColor(90, 170, 255), 2));
            Painter.drawPath(Line);
        }

    private:
        AICore::BTConsideration Curve;
        bool bHas = false;
    };

    // === The canvas =========================================================

    constexpr double BOX_W   = 210.0;
    constexpr double GAP_X   = 26.0;
    constexpr double GAP_Y   = 54.0;
    constexpr double LINE_H  = 16.0;
    constexpr int    ROLE_NODE = 0;

    QStringList BoxLines(const Node& InNode)
    {
        QStringList Lines;
        const AICore::BTNodeDef& Def = InNode.Def;
        if(!Def.Name.empty() && Def.Name != Def.Type) { Lines << QString::fromStdString(Def.Type); }
        for(const AICore::BTParam& Param : Def.Params)
        {
            Lines << QStringLiteral("%1: %2").arg(QString::fromStdString(Param.Name), QString::fromStdString(Param.Text));
        }
        for(const AICore::BTDecoratorDef& D : Def.Decorators) { Lines << DescribeDecorator(D); }
        for(const AICore::BTServiceDef& S : Def.Services)
        {
            Lines << QStringLiteral("⟳ %1 every %2 s").arg(QString::fromStdString(S.Type), FloatText(S.Interval));
        }
        for(const AICore::BTConsideration& C : Def.Considerations)
        {
            Lines << QStringLiteral("∿ %1  %2").arg(QString::fromStdString(C.Input), QString::fromLatin1(AICore::GetBTCurveName(C.Curve)));
        }
        if(InNode.Parent && InNode.Parent->Def.Kind == AICore::EBTNodeKind::Composite
           && (InNode.Parent->Def.Composite == AICore::EBTComposite::RandomSelector || InNode.Parent->Def.Composite == AICore::EBTComposite::UtilitySelector)
           && Def.Weight != 1.0f)
        {
            Lines << QStringLiteral("weight %1").arg(FloatText(Def.Weight));
        }
        return Lines;
    }

    double BoxHeight(const Node& InNode) { return 30.0 + LINE_H * double(BoxLines(InNode).size()) + 6.0; }

    void MeasureDepths(const Node& InNode, const size_t InDepth, std::vector<double>& InOutRows)
    {
        if(InOutRows.size() <= InDepth) { InOutRows.resize(InDepth + 1, 0.0); }
        InOutRows[InDepth] = std::max(InOutRows[InDepth], BoxHeight(InNode));
        for(const std::unique_ptr<Node>& Child : InNode.Children) { MeasureDepths(*Child, InDepth + 1, InOutRows); }
    }

    /* Leaves left to right; a parent centred over its children. Its x. */
    double PlaceX(const Node& InNode, double& InOutNextLeaf, std::unordered_map<const Node*, double>& OutX)
    {
        double X = 0.0;
        if(InNode.Children.empty())
        {
            X = InOutNextLeaf;
            InOutNextLeaf += BOX_W + GAP_X;
        }
        else
        {
            const double First = PlaceX(*InNode.Children.front(), InOutNextLeaf, OutX);
            double Last = First;
            for(size_t Index = 1; Index < InNode.Children.size(); ++Index) { Last = PlaceX(*InNode.Children[Index], InOutNextLeaf, OutX); }
            X = (First + Last) * 0.5;
        }
        OutX[&InNode] = X;
        return X;
    }
}

// ============================================================================
// Opening
// ============================================================================

BehaviorTreeEditorWindow* BehaviorTreeEditorWindow::Open(const QString& InPath, QWidget* InParent)
{
    const QString Absolute = QFileInfo(InPath).absoluteFilePath();
    for(BehaviorTreeEditorWindow* Window : Windows())
    {
        if(QFileInfo(Window->Path) == QFileInfo(Absolute))
        {
            Window->show();
            Window->raise();
            Window->activateWindow();
            return Window;
        }
    }
    auto* Window = new BehaviorTreeEditorWindow(Absolute);
    QString Error;
    if(!Window->Load(Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("Behavior Tree"),
                             QStringLiteral("%1 %2").arg(QDir::toNativeSeparators(Absolute), Error));
        delete Window;
        return nullptr;
    }
    Window->resize(1500, 900);
    Window->show();
    QTimer::singleShot(0, Window, [Window]() { Window->FrameTree(); });   // once the view has its size
    return Window;
}

BehaviorTreeEditorWindow* BehaviorTreeEditorWindow::CreateIn(const QString& InFolder, QWidget* InParent)
{
    const QString Extension = QStringLiteral(".nmbt");
    if(InFolder.isEmpty() || EditorContent::IsEngineContent(InFolder))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Behavior Tree"), QStringLiteral("Pick a folder of the project's content first."));
        return nullptr;
    }
    bool bOk = false;
    QString NewName = QInputDialog::getText(InParent, QStringLiteral("New Behavior Tree"), QStringLiteral("Name:"),
                                            QLineEdit::Normal, QStringLiteral("NewBehaviorTree"), &bOk).trimmed();
    if(!bOk || NewName.isEmpty()) { return nullptr; }
    if(NewName.endsWith(Extension, Qt::CaseInsensitive)) { NewName.chop(Extension.size()); }
    if(NewName.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Behavior Tree"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
        return nullptr;
    }
    QDir().mkpath(InFolder);
    const QString NewPath = QDir(InFolder).filePath(NewName + Extension);
    if(QFileInfo::exists(NewPath))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Behavior Tree"), QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(NewPath)));
        return nullptr;
    }

    /* A Selector over a Wait: the smallest tree that resolves. */
    AICore::BehaviorTreeAsset Asset;
    Asset.Name = NewName.toStdString();
    AICore::BTNodeDef Selector;
    Selector.Kind = AICore::EBTNodeKind::Composite;
    Selector.Composite = AICore::EBTComposite::Selector;
    Selector.Type = AICore::GetBTCompositeName(Selector.Composite);
    Selector.Children = { 1 };
    AICore::BTNodeDef Wait;
    Wait.Kind = AICore::EBTNodeKind::Task;
    Wait.Type = "Wait";
    Wait.Params = MakeParams("Wait");
    Asset.Nodes = { Selector, Wait };
    std::string Error;
    if(!AICore::SaveBehaviorTreeXml(NewPath.toStdString(), Asset, &Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("New Behavior Tree"), QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(NewPath)));
        return nullptr;
    }
    EditorContent::StageAsset(NewPath);
    return Open(NewPath, InParent);
}

BehaviorTreeEditorWindow::BehaviorTreeEditorWindow(const QString& InPath) : QMainWindow(nullptr), Path(InPath)
{
    setAttribute(Qt::WA_DeleteOnClose);
    Windows().push_back(this);
    BuildUi();
}

BehaviorTreeEditorWindow::~BehaviorTreeEditorWindow()
{
    Windows().erase(std::remove(Windows().begin(), Windows().end(), this), Windows().end());
}

bool BehaviorTreeEditorWindow::Load(QString& OutError)
{
    QFile File(Path);
    if(!File.open(QIODevice::ReadOnly)) { OutError = QStringLiteral("could not be read."); return false; }
    const QByteArray Bytes = File.readAll();
    const std::vector<uint8_t> Raw(Bytes.begin(), Bytes.end());
    AICore::BehaviorTreeAsset Asset;
    std::string Error;
    bool bRead = false;
    if(AICore::DetectBehaviorTreeFormat(Raw) == AICore::EBTFileFormat::Binary)
    {
        OutError = QStringLiteral("is a cooked tree: open its source .nmbt.");
        return false;
    }
    bRead = AICore::ParseBehaviorTreeXml(Bytes.toStdString(), Asset, &Error, false);
    if(!bRead) { OutError = QString::fromStdString(Error); return false; }
    if(Asset.Name.empty()) { Asset.Name = QFileInfo(Path).completeBaseName().toStdString(); }
    SetTree(Asset);
    Current = Snapshot();
    UndoStack.clear();
    RedoStack.clear();
    bDirty = false;
    UpdateTitle();
    return true;
}

void BehaviorTreeEditorWindow::SetTree(const AICore::BehaviorTreeAsset& InAsset)
{
    Name = QString::fromStdString(InAsset.Name);
    Keys = InAsset.Keys;
    Root = InAsset.Nodes.empty() ? nullptr : FromAsset(InAsset, 0, nullptr);
    Selected = nullptr;
    {
        bFilling = true;
        TreeName->setText(Name);
        bFilling = false;
    }
    FillBlackboard();
    Rebuild();
    Select(Root.get());
    FrameTree();
}

// ============================================================================
// The window
// ============================================================================

void BehaviorTreeEditorWindow::BuildUi()
{
    QToolBar* Bar = addToolBar(QStringLiteral("Tree"));
    Bar->setObjectName(QStringLiteral("BehaviorTreeToolBar"));
    Bar->setMovable(false);
    const auto Act = [this, Bar](const QString& InText, const QKeySequence& InKey, std::function<void()> InFn)
    {
        QAction* Action = Bar->addAction(InText);
        if(!InKey.isEmpty()) { Action->setShortcut(InKey); Action->setShortcutContext(Qt::WindowShortcut); }
        connect(Action, &QAction::triggered, this, [InFn]() { InFn(); });
        return Action;
    };
    Act(QStringLiteral("Save"), QKeySequence::Save, [this]() { Save(); });
    Bar->addSeparator();
    Act(QStringLiteral("Undo"), QKeySequence::Undo, [this]() { Undo(false); });
    Act(QStringLiteral("Redo"), QKeySequence::Redo, [this]() { Undo(true); });
    Bar->addSeparator();
    Act(QStringLiteral("Add"), QKeySequence(Qt::Key_Insert), [this]() { AddFromPalette(); });
    Act(QStringLiteral("Delete"), QKeySequence::Delete, [this]() { DeleteSelected(); });
    Act(QStringLiteral("Move Up"), QKeySequence(Qt::ALT | Qt::Key_Up), [this]() { MoveSelected(-1); });
    Act(QStringLiteral("Move Down"), QKeySequence(Qt::ALT | Qt::Key_Down), [this]() { MoveSelected(1); });
    Bar->addSeparator();
    Act(QStringLiteral("Cut"), QKeySequence::Cut, [this]() { CopySelected(true); });
    Act(QStringLiteral("Copy"), QKeySequence::Copy, [this]() { CopySelected(false); });
    Act(QStringLiteral("Paste"), QKeySequence::Paste, [this]() { Paste(); });
    Act(QStringLiteral("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this]() { CopySelected(false); Paste(); });
    Bar->addSeparator();
    Act(QStringLiteral("Frame"), QKeySequence(Qt::Key_F), [this]() { FrameTree(); });
    DebugAction = Act(QStringLiteral("Debug in PIE"), QKeySequence(), [this]() { SetDebugging(DebugAction->isChecked()); });
    DebugAction->setCheckable(true);
    DebugAction->setToolTip(QStringLiteral("While Play runs, light the running branch of an agent running this tree"));

    Scene = new QGraphicsScene(this);
    Scene->setBackgroundBrush(QColor(30, 32, 36));
    View = new GraphView(Scene, this);
    View->setRenderHint(QPainter::Antialiasing);
    connect(Scene, &QGraphicsScene::selectionChanged, this, [this]()
    {
        if(bFilling) { return; }
        const QList<QGraphicsItem*> Picked = Scene->selectedItems();
        Node* Pick = Picked.isEmpty() ? nullptr : reinterpret_cast<Node*>(Picked.front()->data(ROLE_NODE).value<quintptr>());
        if(Pick != Selected) { Selected = Pick; FillDetails(); }
    });

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(BuildLeft());
    Split->addWidget(View);
    Split->addWidget(BuildDetails());
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setStretchFactor(2, 0);
    Split->setSizes({ 300, 860, 380 });
    setCentralWidget(Split);

    StatusLine = new QLabel(this);
    DebugLine = new QLabel(this);
    statusBar()->addWidget(StatusLine, 1);
    statusBar()->addPermanentWidget(DebugLine);

    DebugTimer = new QTimer(this);
    DebugTimer->setInterval(200);
    connect(DebugTimer, &QTimer::timeout, this, [this]() { UpdateDebug(); });
}

QWidget* BehaviorTreeEditorWindow::BuildLeft()
{
    auto* Body = new QWidget(this);
    auto* Layout = new QVBoxLayout(Body);
    Layout->setContentsMargins(4, 4, 4, 4);

    auto* NameRow = new QFormLayout();
    TreeName = new QLineEdit(Body);
    NameRow->addRow(QStringLiteral("Tree"), TreeName);
    Layout->addLayout(NameRow);
    connect(TreeName, &QLineEdit::editingFinished, this, [this]()
    {
        if(bFilling || TreeName->text().trimmed() == Name) { return; }
        Name = TreeName->text().trimmed();
        Edited(false);
    });

    Layout->addWidget(new QLabel(QStringLiteral("<b>Blackboard</b>"), Body));
    Blackboard = new QTableWidget(0, 3, Body);
    Blackboard->setHorizontalHeaderLabels({ QStringLiteral("Key"), QStringLiteral("Type"), QStringLiteral("Default") });
    Blackboard->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    Blackboard->verticalHeader()->hide();
    Layout->addWidget(Blackboard, 2);
    connect(Blackboard, &QTableWidget::itemChanged, this, [this]() { if(!bFilling) { ApplyBlackboard(); } });
    auto* KeyButtons = new QHBoxLayout();
    auto* AddKey = new QPushButton(QStringLiteral("+ Key"), Body);
    auto* RemoveKey = new QPushButton(QStringLiteral("- Key"), Body);
    KeyButtons->addWidget(AddKey);
    KeyButtons->addWidget(RemoveKey);
    KeyButtons->addStretch(1);
    Layout->addLayout(KeyButtons);
    connect(AddKey, &QPushButton::clicked, this, [this]()
    {
        AICore::BlackboardKey Key;
        Key.Name = "NewKey" + std::to_string(Keys.size());
        Keys.push_back(Key);
        FillBlackboard();
        Edited(false);
    });
    connect(RemoveKey, &QPushButton::clicked, this, [this]()
    {
        const int Row = Blackboard->currentRow();
        if(Row < 0 || Row >= int(Keys.size())) { return; }
        Keys.erase(Keys.begin() + Row);
        FillBlackboard();
        Edited(false);
    });

    Layout->addWidget(new QLabel(QStringLiteral("<b>Palette</b>"), Body));
    Palette = new QListWidget(Body);
    const auto Header = [this](const QString& InText)
    {
        auto* Item = new QListWidgetItem(InText, Palette);
        Item->setFlags(Qt::NoItemFlags);
        QFont Font = Item->font();
        Font.setBold(true);
        Item->setFont(Font);
    };
    const auto Entry = [this](const QString& InText, const QString& InKind, const std::string& InType)
    {
        auto* Item = new QListWidgetItem(QStringLiteral("   ") + InText, Palette);
        Item->setData(Qt::UserRole, InKind);
        Item->setData(Qt::UserRole + 1, QString::fromStdString(InType));
    };
    Header(QStringLiteral("Composites"));
    for(int Index = 0; Index < int(AICore::EBTComposite::Count); ++Index)
    {
        const char* Composite = AICore::GetBTCompositeName(AICore::EBTComposite(Index));
        Entry(QString::fromLatin1(Composite), QStringLiteral("composite"), Composite);
    }
    Header(QStringLiteral("Tasks"));
    for(const std::string& Task : AICore::GetBTTaskNames())
    {
        Entry(QString::fromStdString(AICore::GetBTTaskDisplayName(Task.c_str())), QStringLiteral("task"), Task);
    }
    Header(QStringLiteral("Decorators"));
    for(const std::string& Decorator : AICore::GetBTDecoratorNames()) { Entry(QString::fromStdString(Decorator), QStringLiteral("decorator"), Decorator); }
    Header(QStringLiteral("Services"));
    for(const std::string& Service : AICore::GetBTServiceNames()) { Entry(QString::fromStdString(Service), QStringLiteral("service"), Service); }
    Palette->setToolTip(QStringLiteral("Double-click: add under the selected node (a decorator or service: onto it)"));
    connect(Palette, &QListWidget::itemDoubleClicked, this, [this]() { AddFromPalette(); });
    Layout->addWidget(Palette, 3);
    return Body;
}

QWidget* BehaviorTreeEditorWindow::BuildDetails()
{
    DetailsBody = new QWidget(this);
    auto* Layout = new QVBoxLayout(DetailsBody);
    Layout->setContentsMargins(4, 4, 4, 4);
    NodeTitle = new QLabel(DetailsBody);
    NodeTitle->setWordWrap(true);
    Layout->addWidget(NodeTitle);

    auto* Form = new QFormLayout();
    NodeName = new QLineEdit(DetailsBody);
    NodeName->setPlaceholderText(QStringLiteral("(its type)"));
    NodeWeight = new QDoubleSpinBox(DetailsBody);
    NodeWeight->setRange(0.0, 1000.0);
    NodeWeight->setDecimals(3);
    NodeWeight->setSingleStep(0.1);
    NodeWeight->setToolTip(QStringLiteral("Its pick weight under a Random Selector, its score multiplier under a Utility Selector"));
    Form->addRow(QStringLiteral("Name"), NodeName);
    Form->addRow(QStringLiteral("Weight"), NodeWeight);
    Layout->addLayout(Form);
    connect(NodeName, &QLineEdit::editingFinished, this, [this]() { if(!bFilling) { ApplyDetails(); } });
    connect(NodeWeight, &QDoubleSpinBox::editingFinished, this, [this]() { if(!bFilling) { ApplyDetails(); } });

    const auto Table = [this, Layout](const QString& InTitle, const QStringList& InColumns, QTableWidget*& OutTable, std::function<void()> InAdd)
    {
        auto* Row = new QHBoxLayout();
        Row->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(InTitle), DetailsBody));
        Row->addStretch(1);
        auto* Add = new QPushButton(QStringLiteral("+"), DetailsBody);
        auto* Remove = new QPushButton(QStringLiteral("-"), DetailsBody);
        Add->setFixedWidth(28);
        Remove->setFixedWidth(28);
        Row->addWidget(Add);
        Row->addWidget(Remove);
        Layout->addLayout(Row);
        OutTable = new QTableWidget(0, int(InColumns.size()), DetailsBody);
        OutTable->setHorizontalHeaderLabels(InColumns);
        OutTable->horizontalHeader()->setStretchLastSection(true);
        OutTable->verticalHeader()->hide();
        Layout->addWidget(OutTable, 1);
        QTableWidget* Target = OutTable;
        connect(Target, &QTableWidget::itemChanged, this, [this]() { if(!bFilling) { ApplyDetails(); } });
        connect(Add, &QPushButton::clicked, this, [this, InAdd]() { if(Selected) { InAdd(); FillDetails(); Edited(false); } });
        connect(Remove, &QPushButton::clicked, this, [this, Target]()
        {
            const int RowIndex = Target->currentRow();
            if(!Selected || RowIndex < 0) { return; }
            Target->removeRow(RowIndex);
            ApplyDetails();
            FillDetails();
        });
    };
    Table(QStringLiteral("Params"), { QStringLiteral("Name"), QStringLiteral("Value") }, Params, [this]()
    {
        AICore::BTParam Param;
        Param.Name = "Param";
        Selected->Def.Params.push_back(Param);
    });
    Table(QStringLiteral("Decorators"), { QStringLiteral("Type"), QStringLiteral("Abort"), QStringLiteral("Params") }, Decorators, [this]()
    {
        const std::vector<std::string> Names = AICore::GetBTDecoratorNames();
        AICore::BTDecoratorDef Decorator;
        Decorator.Type = Names.empty() ? std::string("BlackboardCondition") : Names.front();
        Decorator.Params = MakeParams(Decorator.Type);
        Selected->Def.Decorators.push_back(Decorator);
    });
    Table(QStringLiteral("Services"), { QStringLiteral("Type"), QStringLiteral("Every (s)"), QStringLiteral("Params") }, Services, [this]()
    {
        const std::vector<std::string> Names = AICore::GetBTServiceNames();
        AICore::BTServiceDef Service;
        Service.Type = Names.empty() ? std::string("DistanceToKey") : Names.front();
        Service.Params = MakeParams(Service.Type);
        Selected->Def.Services.push_back(Service);
    });
    Table(QStringLiteral("Considerations"),
          { QStringLiteral("Input"), QStringLiteral("Min"), QStringLiteral("Max"), QStringLiteral("Curve"), QStringLiteral("Slope"),
            QStringLiteral("Exp"), QStringLiteral("XShift"), QStringLiteral("YShift"), QStringLiteral("Weight"), QStringLiteral("Points") },
          Considerations, [this]()
    {
        AICore::BTConsideration Consideration;
        Consideration.Input = Keys.empty() ? std::string() : Keys.front().Name;
        Selected->Def.Considerations.push_back(Consideration);
    });
    connect(Considerations, &QTableWidget::currentCellChanged, this, [this]() { FillConsiderationCurve(); });
    Considerations->setToolTip(QStringLiteral("A Utility Selector child's inputs: a key, or distance(Key); Min..Max maps onto 0..1, "
                                              "the curve shapes it, the score is raised to Weight. Points: x0 y0 x1 y1 ..."));
    Curve = new CurvePreview(DetailsBody);
    Layout->addWidget(Curve);
    return DetailsBody;
}

// ============================================================================
// The tree <-> the asset
// ============================================================================

AICore::BehaviorTreeAsset BehaviorTreeEditorWindow::Flatten()
{
    AICore::BehaviorTreeAsset Asset;
    Asset.Name = Name.toStdString();
    Asset.Keys = Keys;
    if(Root) { FlattenInto(*Root, Asset); }
    return Asset;
}

std::string BehaviorTreeEditorWindow::Snapshot()
{
    return AICore::WriteBehaviorTreeXml(Flatten());
}

void BehaviorTreeEditorWindow::Edited(const bool bInRefillDetails)
{
    const std::string Now = Snapshot();
    if(Now != Current)
    {
        UndoStack.push_back(Current);
        if(UndoStack.size() > 200) { UndoStack.erase(UndoStack.begin()); }
        RedoStack.clear();
        Current = Now;
        bDirty = true;
    }
    Rebuild();
    if(bInRefillDetails) { FillDetails(); }
    UpdateTitle();
}

void BehaviorTreeEditorWindow::Undo(const bool bInRedo)
{
    std::vector<std::string>& From = bInRedo ? RedoStack : UndoStack;
    std::vector<std::string>& To   = bInRedo ? UndoStack : RedoStack;
    if(From.empty()) { return; }
    const int Keep = Selected ? Selected->Flat : 0;
    To.push_back(Current);
    Current = From.back();
    From.pop_back();
    AICore::BehaviorTreeAsset Asset;
    if(AICore::ParseBehaviorTreeXml(Current, Asset, nullptr, false))
    {
        Name = QString::fromStdString(Asset.Name);
        Keys = Asset.Keys;
        Root = Asset.Nodes.empty() ? nullptr : FromAsset(Asset, 0, nullptr);
        Selected = nullptr;
        bFilling = true;
        TreeName->setText(Name);
        bFilling = false;
        FillBlackboard();
        Rebuild();
        Select(FindFlat(Root.get(), Keep) ? FindFlat(Root.get(), Keep) : Root.get());
    }
    bDirty = true;
    UpdateTitle();
}

bool BehaviorTreeEditorWindow::Save()
{
    AICore::BehaviorTreeAsset Asset = Flatten();
    if(Asset.Nodes.empty())
    {
        QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("The tree has no nodes: add a composite from the palette first."));
        return false;
    }
    std::string Error;
    if(!AICore::SaveBehaviorTreeXml(Path.toStdString(), Asset, &Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("%1 %2").arg(QDir::toNativeSeparators(Path), QString::fromStdString(Error)));
        return false;
    }
    EditorContent::StageAsset(Path);
    AICore::BehaviorTreeAssetCache::Invalidate(EditorContent::ToRuntimePath(Path).toStdString());
    bDirty = false;
    UpdateTitle();
    statusBar()->showMessage(QStringLiteral("Saved; running brains restart on it"), 4000);
    return true;
}

void BehaviorTreeEditorWindow::UpdateTitle()
{
    setWindowTitle(QStringLiteral("%1%2 - Behavior Tree").arg(QFileInfo(Path).fileName(), bDirty ? QStringLiteral(" *") : QString()));
    AICore::BehaviorTreeAsset Asset = Flatten();
    std::string Error;
    if(Asset.Nodes.empty())          { StatusLine->setText(QStringLiteral("Empty: add a composite from the palette")); }
    else if(!Asset.Resolve(&Error))  { StatusLine->setText(QStringLiteral("<span style='color:#ff7a6a'>%1</span>").arg(QString::fromStdString(Error).toHtmlEscaped())); }
    else                             { StatusLine->setText(QStringLiteral("%1 node(s), %2 key(s): resolves").arg(Asset.Nodes.size()).arg(Asset.Keys.size())); }
}

void BehaviorTreeEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    if(bDirty)
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(this, QStringLiteral("Behavior Tree"),
            QStringLiteral("Save the changes to %1?").arg(QFileInfo(Path).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if(Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !Save()))
        {
            InEvent->ignore();
            return;
        }
    }
    InEvent->accept();
}

// ============================================================================
// The canvas
// ============================================================================

void BehaviorTreeEditorWindow::Rebuild()
{
    bFilling = true;
    Scene->clear();
    Boxes.clear();
    Scores.clear();
    if(!Root)
    {
        Scene->addSimpleText(QStringLiteral("Empty tree: double-click a composite in the palette"))->setBrush(QColor(170, 170, 170));
        bFilling = false;
        return;
    }
    Flatten();   // the Flat indices, for the debugger

    std::vector<double> Rows;
    MeasureDepths(*Root, 0, Rows);
    std::vector<double> RowY(Rows.size(), 0.0);
    for(size_t Depth = 1; Depth < Rows.size(); ++Depth) { RowY[Depth] = RowY[Depth - 1] + Rows[Depth - 1] + GAP_Y; }
    std::unordered_map<const Node*, double> X;
    double NextLeaf = 0.0;
    PlaceX(*Root, NextLeaf, X);

    QFont Title = Scene->font();
    Title.setBold(true);
    QFont Small = Scene->font();
    Small.setPointSizeF(Small.pointSizeF() * 0.9);
    const QFontMetrics SmallMetrics(Small);

    std::function<void(const Node&, size_t)> Draw = [&](const Node& InNode, const size_t InDepth)
    {
        const double Left = X[&InNode];
        const double Top = RowY[InDepth];
        const double Height = BoxHeight(InNode);
        const bool bComposite = IsComposite(InNode);
        const bool bKnown = IsKnown(InNode);

        auto* Box = new QGraphicsRectItem(0, 0, BOX_W, Height);
        Box->setPos(Left, Top);
        Box->setBrush(bComposite ? QColor(52, 58, 74) : QColor(40, 66, 64));
        Box->setPen(QPen(bKnown ? QColor(90, 96, 110) : QColor(230, 90, 80), bKnown ? 1.0 : 2.0));
        Box->setFlag(QGraphicsItem::ItemIsSelectable);
        Box->setData(ROLE_NODE, QVariant::fromValue<quintptr>(quintptr(&InNode)));
        Box->setToolTip(bKnown ? QString() : QStringLiteral("A type this editor has not registered"));
        Scene->addItem(Box);
        Boxes[&InNode] = Box;

        auto* Head = new QGraphicsRectItem(0, 0, BOX_W, 24, Box);
        Head->setBrush(bComposite ? QColor(74, 86, 120) : QColor(46, 110, 100));
        Head->setPen(Qt::NoPen);
        Head->setAcceptedMouseButtons(Qt::NoButton);
        auto* Text = new QGraphicsSimpleTextItem(QFontMetrics(Title).elidedText(QString::fromUtf8(InNode.Def.GetDisplayName()), Qt::ElideRight, int(BOX_W - 34)), Head);
        Text->setFont(Title);
        Text->setBrush(Qt::white);
        Text->setPos(8, 4);
        if(InNode.Parent)
        {
            /* Its order: a Selector's priority, a Sequence's step. */
            const auto& Siblings = InNode.Parent->Children;
            const auto At = std::find_if(Siblings.begin(), Siblings.end(), [&](const std::unique_ptr<Node>& InS) { return InS.get() == &InNode; });
            auto* Order = new QGraphicsSimpleTextItem(QString::number(int(At - Siblings.begin()) + 1), Head);
            Order->setFont(Small);
            Order->setBrush(QColor(200, 210, 230));
            Order->setPos(BOX_W - 8 - SmallMetrics.horizontalAdvance(Order->text()), 5);
        }
        double LineY = 28.0;
        for(const QString& Line : BoxLines(InNode))
        {
            auto* Item = new QGraphicsSimpleTextItem(SmallMetrics.elidedText(Line, Qt::ElideRight, int(BOX_W - 14)), Box);
            Item->setFont(Small);
            Item->setBrush(Line.startsWith(QChar(0x25C7)) ? QColor(240, 200, 120)
                         : Line.startsWith(QChar(0x27F3)) ? QColor(150, 210, 250)
                         : Line.startsWith(QChar(0x223F)) ? QColor(190, 160, 250)
                                                          : QColor(205, 205, 205));
            Item->setPos(7, LineY);
            LineY += LINE_H;
        }
        auto* Score = new QGraphicsSimpleTextItem(QString(), Box);
        Score->setFont(Title);
        Score->setBrush(QColor(120, 200, 255));
        Score->setPos(0, Height + 2);
        Scores[&InNode] = Score;

        for(const std::unique_ptr<Node>& Child : InNode.Children)
        {
            const double ChildTop = RowY[InDepth + 1];
            const double Mid = Top + Rows[InDepth] + GAP_Y * 0.5;
            QPainterPath Wire(QPointF(Left + BOX_W * 0.5, Top + Height));
            Wire.lineTo(Left + BOX_W * 0.5, Mid);
            Wire.lineTo(X[Child.get()] + BOX_W * 0.5, Mid);
            Wire.lineTo(X[Child.get()] + BOX_W * 0.5, ChildTop);
            auto* WireItem = Scene->addPath(Wire, QPen(QColor(120, 126, 140), 1.5));
            WireItem->setZValue(-1);
            Draw(*Child, InDepth + 1);
        }
    };
    Draw(*Root, 0);

    if(Selected && Boxes.count(Selected)) { Boxes[Selected]->setSelected(true); }
    bFilling = false;
    for(auto& [Each, Box] : Boxes)
    {
        if(Box->isSelected()) { Box->setPen(QPen(QColor(255, 210, 90), 2.5)); }
    }
    UpdateDebug();
}

void BehaviorTreeEditorWindow::FrameTree()
{
    /* All of it if that stays readable; else readable, from the root down. */
    const QRectF Bounds = Scene->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    const QSize Port = View->viewport()->size();
    if(Bounds.isEmpty() || Port.isEmpty()) { return; }
    const double Fit = std::min({ 1.0, double(Port.width()) / Bounds.width(), double(Port.height()) / Bounds.height() });
    const double Scale = std::max(Fit, 0.6);
    View->resetTransform();
    View->scale(Scale, Scale);
    if(Fit >= 0.6 || !Root || !Boxes.count(Root.get())) { View->centerOn(Bounds.center()); return; }
    View->centerOn(Boxes[Root.get()]->sceneBoundingRect().center().x(), Bounds.top() + double(Port.height()) / (2.0 * Scale));
}

bool BehaviorTreeEditorWindow::SelectByName(const QString& InName)
{
    const std::string Wanted = InName.toStdString();
    std::function<Node*(Node*)> Find = [&](Node* InNode) -> Node*
    {
        if(!InNode) { return nullptr; }
        if(InNode->Def.GetDisplayName() == Wanted) { return InNode; }
        for(const std::unique_ptr<Node>& Child : InNode->Children)
        {
            if(Node* Found = Find(Child.get())) { return Found; }
        }
        return nullptr;
    };
    Node* Found = Find(Root.get());
    if(!Found) { return false; }
    Select(Found);
    if(Boxes.count(Found)) { View->centerOn(Boxes[Found]); }
    return true;
}

void BehaviorTreeEditorWindow::Select(Node* InNode)
{
    Selected = InNode;
    bFilling = true;
    Scene->clearSelection();
    if(InNode && Boxes.count(InNode)) { Boxes[InNode]->setSelected(true); View->ensureVisible(Boxes[InNode]); }
    bFilling = false;
    FillDetails();
}

// ============================================================================
// Editing the tree
// ============================================================================

void BehaviorTreeEditorWindow::Insert(std::unique_ptr<Node> InNode)
{
    Node* Added = InNode.get();
    if(!Root)
    {
        InNode->Parent = nullptr;
        Root = std::move(InNode);
    }
    else
    {
        Node* Target = Selected ? Selected : Root.get();
        if(IsComposite(*Target))
        {
            InNode->Parent = Target;
            Target->Children.push_back(std::move(InNode));
        }
        else if(Target->Parent)
        {
            /* After a task: its next sibling. */
            Node* Parent = Target->Parent;
            auto At = std::find_if(Parent->Children.begin(), Parent->Children.end(), [&](const std::unique_ptr<Node>& InS) { return InS.get() == Target; });
            InNode->Parent = Parent;
            Parent->Children.insert(At + 1, std::move(InNode));
        }
        else
        {
            statusBar()->showMessage(QStringLiteral("The root is a task: make a composite the root first"), 4000);
            return;
        }
    }
    Selected = Added;
    Edited();
    Select(Added);
}

void BehaviorTreeEditorWindow::AddFromPalette()
{
    QListWidgetItem* Item = Palette->currentItem();
    if(!Item || !Item->data(Qt::UserRole).isValid()) { statusBar()->showMessage(QStringLiteral("Pick something in the palette"), 3000); return; }
    const QString Kind = Item->data(Qt::UserRole).toString();
    const std::string Type = Item->data(Qt::UserRole + 1).toString().toStdString();
    if(Kind == QLatin1String("decorator") || Kind == QLatin1String("service"))
    {
        if(!Selected) { statusBar()->showMessage(QStringLiteral("Select the node to put it on"), 3000); return; }
        if(Kind == QLatin1String("decorator"))
        {
            AICore::BTDecoratorDef Decorator;
            Decorator.Type = Type;
            Decorator.Params = MakeParams(Type);
            Selected->Def.Decorators.push_back(Decorator);
        }
        else
        {
            if(!IsComposite(*Selected)) { statusBar()->showMessage(QStringLiteral("A service goes on a composite"), 3000); return; }
            AICore::BTServiceDef Service;
            Service.Type = Type;
            Service.Params = MakeParams(Type);
            Selected->Def.Services.push_back(Service);
        }
        Edited();
        return;
    }
    auto Added = std::make_unique<Node>();
    if(Kind == QLatin1String("composite"))
    {
        Added->Def.Kind = AICore::EBTNodeKind::Composite;
        Added->Def.Composite = AICore::FindBTComposite(Type.c_str());
    }
    else
    {
        Added->Def.Kind = AICore::EBTNodeKind::Task;
    }
    Added->Def.Type = Type;
    Added->Def.Params = MakeParams(Type);
    Insert(std::move(Added));
}

void BehaviorTreeEditorWindow::DeleteSelected()
{
    if(!Selected) { return; }
    if(!Selected->Parent)
    {
        Root.reset();
        Selected = nullptr;
        Edited();
        return;
    }
    Node* Parent = Selected->Parent;
    auto& Siblings = Parent->Children;
    Siblings.erase(std::remove_if(Siblings.begin(), Siblings.end(), [this](const std::unique_ptr<Node>& InS) { return InS.get() == Selected; }), Siblings.end());
    Selected = Parent;
    Edited();
    Select(Parent);
}

void BehaviorTreeEditorWindow::MoveSelected(const int InBy)
{
    if(!Selected || !Selected->Parent) { return; }
    auto& Siblings = Selected->Parent->Children;
    const auto At = std::find_if(Siblings.begin(), Siblings.end(), [this](const std::unique_ptr<Node>& InS) { return InS.get() == Selected; });
    const long Index = long(At - Siblings.begin());
    const long To = Index + InBy;
    if(To < 0 || To >= long(Siblings.size())) { return; }
    std::swap(Siblings[size_t(Index)], Siblings[size_t(To)]);
    Edited();
    Select(Selected);
}

void BehaviorTreeEditorWindow::CopySelected(const bool bInCut)
{
    if(!Selected) { return; }
    Clipboard = Clone(*Selected, nullptr);
    if(bInCut) { DeleteSelected(); }
}

void BehaviorTreeEditorWindow::Paste()
{
    if(!Clipboard) { return; }
    Insert(Clone(*Clipboard, nullptr));
}

// ============================================================================
// The panes
// ============================================================================

void BehaviorTreeEditorWindow::FillBlackboard()
{
    bFilling = true;
    Blackboard->setRowCount(int(Keys.size()));
    for(int Row = 0; Row < int(Keys.size()); ++Row)
    {
        const AICore::BlackboardKey& Key = Keys[size_t(Row)];
        Blackboard->setItem(Row, 0, new QTableWidgetItem(QString::fromStdString(Key.Name)));
        auto* Type = new QComboBox(Blackboard);
        for(int Index = 0; Index < int(AICore::EBlackboardType::Count); ++Index)
        {
            Type->addItem(QString::fromLatin1(AICore::GetBlackboardTypeName(AICore::EBlackboardType(Index))));
        }
        Type->setCurrentIndex(int(Key.Type));
        connect(Type, &QComboBox::currentIndexChanged, this, [this]() { if(!bFilling) { ApplyBlackboard(); } });
        Blackboard->setCellWidget(Row, 1, Type);
        Blackboard->setItem(Row, 2, new QTableWidgetItem(QString::fromStdString(Key.Default)));
    }
    bFilling = false;
}

void BehaviorTreeEditorWindow::ApplyBlackboard()
{
    for(int Row = 0; Row < Blackboard->rowCount() && Row < int(Keys.size()); ++Row)
    {
        AICore::BlackboardKey& Key = Keys[size_t(Row)];
        if(QTableWidgetItem* KeyName = Blackboard->item(Row, 0)) { Key.Name = KeyName->text().trimmed().toStdString(); }
        if(auto* Type = qobject_cast<QComboBox*>(Blackboard->cellWidget(Row, 1))) { Key.Type = AICore::EBlackboardType(Type->currentIndex()); }
        if(QTableWidgetItem* Default = Blackboard->item(Row, 2)) { Key.Default = Default->text().trimmed().toStdString(); }
    }
    Edited(false);
}

void BehaviorTreeEditorWindow::FillDetails()
{
    bFilling = true;
    DetailsBody->setEnabled(Selected != nullptr);
    Params->setRowCount(0);
    Decorators->setRowCount(0);
    Services->setRowCount(0);
    Considerations->setRowCount(0);
    if(!Selected)
    {
        NodeTitle->setText(QStringLiteral("<i>Nothing selected</i>"));
        NodeName->clear();
        static_cast<CurvePreview*>(Curve)->SetCurve(nullptr);
        bFilling = false;
        return;
    }
    const AICore::BTNodeDef& Def = Selected->Def;
    const bool bComposite = IsComposite(*Selected);
    NodeTitle->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>%2%3</span>")
                       .arg(QString::fromStdString(Def.Type).toHtmlEscaped(), bComposite ? QStringLiteral("composite") : QStringLiteral("task"),
                            IsKnown(*Selected) ? QString() : QStringLiteral(", not registered here")));
    NodeName->setText(QString::fromStdString(Def.Name));
    NodeWeight->setValue(double(Def.Weight));

    Params->setRowCount(int(Def.Params.size()));
    for(int Row = 0; Row < int(Def.Params.size()); ++Row)
    {
        Params->setItem(Row, 0, new QTableWidgetItem(QString::fromStdString(Def.Params[size_t(Row)].Name)));
        Params->setItem(Row, 1, new QTableWidgetItem(QString::fromStdString(Def.Params[size_t(Row)].Text)));
    }

    const auto TypeCombo = [this](QTableWidget* InTable, const std::vector<std::string>& InNames, const std::string& InCurrent)
    {
        auto* Combo = new QComboBox(InTable);
        for(const std::string& Each : InNames) { Combo->addItem(QString::fromStdString(Each)); }
        if(Combo->findText(QString::fromStdString(InCurrent)) < 0) { Combo->addItem(QString::fromStdString(InCurrent)); }
        Combo->setCurrentText(QString::fromStdString(InCurrent));
        connect(Combo, &QComboBox::currentIndexChanged, this, [this]() { if(!bFilling) { ApplyDetails(); } });
        return Combo;
    };
    const std::vector<std::string> DecoratorNames = AICore::GetBTDecoratorNames();
    Decorators->setRowCount(int(Def.Decorators.size()));
    for(int Row = 0; Row < int(Def.Decorators.size()); ++Row)
    {
        const AICore::BTDecoratorDef& D = Def.Decorators[size_t(Row)];
        Decorators->setCellWidget(Row, 0, TypeCombo(Decorators, DecoratorNames, D.Type));
        auto* Abort = new QComboBox(Decorators);
        for(int Mode = 0; Mode <= int(AICore::EBTAbortMode::Both); ++Mode)
        {
            Abort->addItem(QString::fromLatin1(AICore::GetBTAbortModeName(AICore::EBTAbortMode(Mode))));
        }
        Abort->setCurrentIndex(int(D.Abort));
        Abort->setToolTip(QStringLiteral("Self: stop the node when the condition turns false.\n"
                                         "LowerPriority: under a Selector, switch back to it when the condition turns true."));
        connect(Abort, &QComboBox::currentIndexChanged, this, [this]() { if(!bFilling) { ApplyDetails(); } });
        Decorators->setCellWidget(Row, 1, Abort);
        Decorators->setItem(Row, 2, new QTableWidgetItem(ParamsText(D.Params)));
    }

    const std::vector<std::string> ServiceNames = AICore::GetBTServiceNames();
    Services->setRowCount(int(Def.Services.size()));
    for(int Row = 0; Row < int(Def.Services.size()); ++Row)
    {
        const AICore::BTServiceDef& S = Def.Services[size_t(Row)];
        Services->setCellWidget(Row, 0, TypeCombo(Services, ServiceNames, S.Type));
        Services->setItem(Row, 1, new QTableWidgetItem(FloatText(S.Interval)));
        Services->setItem(Row, 2, new QTableWidgetItem(ParamsText(S.Params)));
    }

    Considerations->setRowCount(int(Def.Considerations.size()));
    for(int Row = 0; Row < int(Def.Considerations.size()); ++Row)
    {
        const AICore::BTConsideration& C = Def.Considerations[size_t(Row)];
        Considerations->setItem(Row, 0, new QTableWidgetItem(QString::fromStdString(C.Input)));
        Considerations->setItem(Row, 1, new QTableWidgetItem(FloatText(C.Min)));
        Considerations->setItem(Row, 2, new QTableWidgetItem(FloatText(C.Max)));
        auto* CurveType = new QComboBox(Considerations);
        for(int Index = 0; Index < int(AICore::EBTCurve::Count); ++Index) { CurveType->addItem(QString::fromLatin1(AICore::GetBTCurveName(AICore::EBTCurve(Index)))); }
        CurveType->setCurrentIndex(int(C.Curve));
        connect(CurveType, &QComboBox::currentIndexChanged, this, [this]() { if(!bFilling) { ApplyDetails(); } });
        Considerations->setCellWidget(Row, 3, CurveType);
        Considerations->setItem(Row, 4, new QTableWidgetItem(FloatText(C.Slope)));
        Considerations->setItem(Row, 5, new QTableWidgetItem(FloatText(C.Exponent)));
        Considerations->setItem(Row, 6, new QTableWidgetItem(FloatText(C.XShift)));
        Considerations->setItem(Row, 7, new QTableWidgetItem(FloatText(C.YShift)));
        Considerations->setItem(Row, 8, new QTableWidgetItem(FloatText(C.Weight)));
        QStringList Points;
        for(const float P : C.Points) { Points << FloatText(P); }
        Considerations->setItem(Row, 9, new QTableWidgetItem(Points.join(QLatin1Char(' '))));
    }
    if(Considerations->rowCount() > 0) { Considerations->setCurrentCell(0, 0); }
    bFilling = false;
    FillConsiderationCurve();
}

void BehaviorTreeEditorWindow::FillConsiderationCurve()
{
    auto* Preview = static_cast<CurvePreview*>(Curve);
    const int Row = Considerations->currentRow();
    if(!Selected || Row < 0 || Row >= int(Selected->Def.Considerations.size())) { Preview->SetCurve(nullptr); return; }
    Preview->SetCurve(&Selected->Def.Considerations[size_t(Row)]);
}

void BehaviorTreeEditorWindow::ApplyDetails()
{
    if(!Selected) { return; }
    AICore::BTNodeDef& Def = Selected->Def;
    Def.Name = NodeName->text().trimmed().toStdString();
    Def.Weight = float(NodeWeight->value());

    const auto Cell = [](QTableWidget* InTable, const int InRow, const int InColumn)
    {
        const QTableWidgetItem* Item = InTable->item(InRow, InColumn);
        return Item ? Item->text().trimmed() : QString();
    };
    const auto Combo = [](QTableWidget* InTable, const int InRow, const int InColumn) { return qobject_cast<QComboBox*>(InTable->cellWidget(InRow, InColumn)); };
    const auto Number = [&](QTableWidget* InTable, const int InRow, const int InColumn, const float InDefault)
    {
        bool bOk = false;
        const float Value = Cell(InTable, InRow, InColumn).toFloat(&bOk);
        return bOk ? Value : InDefault;
    };

    Def.Params.clear();
    for(int Row = 0; Row < Params->rowCount(); ++Row)
    {
        AICore::BTParam Param;
        Param.Name = Cell(Params, Row, 0).toStdString();
        Param.Text = Cell(Params, Row, 1).toStdString();
        if(!Param.Name.empty()) { Def.Params.push_back(Param); }
    }

    std::vector<AICore::BTDecoratorDef> NewDecorators;
    for(int Row = 0; Row < Decorators->rowCount(); ++Row)
    {
        AICore::BTDecoratorDef D;
        if(QComboBox* Type = Combo(Decorators, Row, 0)) { D.Type = Type->currentText().toStdString(); }
        if(QComboBox* Abort = Combo(Decorators, Row, 1)) { D.Abort = AICore::EBTAbortMode(Abort->currentIndex()); }
        D.Params = ParseParamsText(Cell(Decorators, Row, 2));
        /* A new type: the params its code reads. */
        if(Row < int(Def.Decorators.size()) && Def.Decorators[size_t(Row)].Type != D.Type) { D.Params = MakeParams(D.Type); }
        NewDecorators.push_back(D);
    }
    Def.Decorators = NewDecorators;

    std::vector<AICore::BTServiceDef> NewServices;
    for(int Row = 0; Row < Services->rowCount(); ++Row)
    {
        AICore::BTServiceDef S;
        if(QComboBox* Type = Combo(Services, Row, 0)) { S.Type = Type->currentText().toStdString(); }
        S.Interval = std::max(0.0f, Number(Services, Row, 1, 0.5f));
        S.Params = ParseParamsText(Cell(Services, Row, 2));
        if(Row < int(Def.Services.size()) && Def.Services[size_t(Row)].Type != S.Type) { S.Params = MakeParams(S.Type); }
        NewServices.push_back(S);
    }
    Def.Services = NewServices;

    std::vector<AICore::BTConsideration> NewConsiderations;
    for(int Row = 0; Row < Considerations->rowCount(); ++Row)
    {
        AICore::BTConsideration C;
        C.Input    = Cell(Considerations, Row, 0).toStdString();
        C.Min      = Number(Considerations, Row, 1, 0.0f);
        C.Max      = Number(Considerations, Row, 2, 1.0f);
        if(QComboBox* CurveType = Combo(Considerations, Row, 3)) { C.Curve = AICore::EBTCurve(CurveType->currentIndex()); }
        C.Slope    = Number(Considerations, Row, 4, 1.0f);
        C.Exponent = Number(Considerations, Row, 5, 2.0f);
        C.XShift   = Number(Considerations, Row, 6, 0.0f);
        C.YShift   = Number(Considerations, Row, 7, 0.0f);
        C.Weight   = Number(Considerations, Row, 8, 1.0f);
        for(const QString& Point : Cell(Considerations, Row, 9).split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts))
        {
            C.Points.push_back(Point.toFloat());
        }
        NewConsiderations.push_back(C);
    }
    Def.Considerations = NewConsiderations;

    /* A changed type refills its params; anything else keeps the pane as typed. */
    const bool bRefill = Decorators->rowCount() != int(Def.Decorators.size()) || Services->rowCount() != int(Def.Services.size());
    Edited(bRefill);
    FillConsiderationCurve();
}

// ============================================================================
// Debug in PIE
// ============================================================================

void BehaviorTreeEditorWindow::SetDebugging(const bool bInOn)
{
    DebugAction->setChecked(bInOn);
    if(bInOn) { DebugTimer->start(); } else { DebugTimer->stop(); }
    UpdateDebug();
}

void BehaviorTreeEditorWindow::UpdateDebug()
{
    /* Everything back to its edit look first. */
    for(auto& [Each, Box] : Boxes)
    {
        const bool bKnown = IsKnown(*Each);
        Box->setPen(Each == Selected ? QPen(QColor(255, 210, 90), 2.5) : QPen(bKnown ? QColor(90, 96, 110) : QColor(230, 90, 80), bKnown ? 1.0 : 2.0));
    }
    for(auto& [Each, Score] : Scores) { Score->setText(QString()); }
    if(!DebugAction || !DebugAction->isChecked()) { DebugLine->clear(); return; }

    AICore::AIWorld* AI = EditorAI::IsPlaying() ? EditorAI::GetAI() : nullptr;
    if(!AI) { DebugLine->setText(QStringLiteral("Debug: waiting for Play")); return; }

    /* The debugger's agent if it runs this tree, else the first that does. */
    const std::string TreeName0 = Name.toStdString();
    AICore::BTInstance* Brain = nullptr;
    GameCore::EntityID Agent;
    if(AICore::BTInstance* Picked = AI->FindBrain(EditorAI::GetDebuggedAgent()); Picked && Picked->GetAsset().Name == TreeName0)
    {
        Brain = Picked;
        Agent = EditorAI::GetDebuggedAgent();
    }
    if(!Brain)
    {
        AI->ForEachBrain([&](const GameCore::EntityID InEntity, AICore::BTInstance& InBrain)
        {
            if(!Brain && InBrain.GetAsset().Name == TreeName0) { Brain = &InBrain; Agent = InEntity; }
        });
    }
    if(!Brain) { DebugLine->setText(QStringLiteral("Debug: no agent runs %1").arg(Name)); return; }

    const AICore::BehaviorTreeAsset& Running = Brain->GetAsset();
    std::vector<uint32_t> Path;
    Brain->GetActivePath(Path);
    for(auto& [Each, Box] : Boxes)
    {
        if(Each->Flat < 0 || size_t(Each->Flat) >= Running.Nodes.size()) { continue; }
        const uint32_t Flat = uint32_t(Each->Flat);
        if(std::find(Path.begin(), Path.end(), Flat) != Path.end()) { Box->setPen(QPen(QColor(80, 170, 255), 3.5)); }
        const float Score = Brain->GetUtilityScore(Flat);
        if(Score >= 0.0f && Each->Parent && Each->Parent->Def.Composite == AICore::EBTComposite::UtilitySelector)
        {
            Scores[Each]->setText(QStringLiteral("score %1").arg(double(Score), 0, 'f', 2));
        }
    }
    DebugLine->setText(QStringLiteral("Debug: agent #%1%2").arg(Agent.Index)
                       .arg(bDirty ? QStringLiteral(" (edited since saved: the lit branch may be off)") : QString()));
}

#endif // WITH_EDITOR && WITH_AICORE
