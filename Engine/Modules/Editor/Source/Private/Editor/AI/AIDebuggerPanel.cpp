#include "Private/Editor/AI/AIDebuggerPanel.h"

#if WITH_EDITOR && WITH_AICORE

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QMainWindow>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Private/Editor/AI/BehaviorTreeEditorWindow.h"
#include "Private/Editor/AI/EditorAI.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Public/AICore/Components/AIBrainComponent.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* AIDebuggerPanel.h has the dock's layout. */
namespace
{
    AIDebuggerPanel*& Instance()
    {
        static AIDebuggerPanel* Panel = nullptr;
        return Panel;
    }

    QString Vec(const EngineCore::Vector3& InV)
    {
        return QStringLiteral("%1, %2, %3").arg(double(InV.GetX()), 0, 'f', 0).arg(double(InV.GetY()), 0, 'f', 0).arg(double(InV.GetZ()), 0, 'f', 0);
    }

    QString NameOf(GameCore::World& InWorld, const GameCore::EntityID InEntity)
    {
        if(!InEntity.IsSet() || !InWorld.IsAlive(InEntity)) { return QStringLiteral("none"); }
        const QString Name = QString::fromUtf8(InWorld.GetEntity(InEntity).GetName());
        return Name.isEmpty() ? QStringLiteral("#%1").arg(InEntity.Index) : Name;
    }
}

AIDebuggerPanel& AIDebuggerPanel::Install(QMainWindow& InHost, QDockWidget* InTabifyWith)
{
    if(Instance()) { return *Instance(); }
    auto* Dock = new AIDebuggerPanel(&InHost);
    Instance() = Dock;
    InHost.addDockWidget(Qt::RightDockWidgetArea, Dock);
    if(InTabifyWith) { InHost.tabifyDockWidget(InTabifyWith, Dock); }
    Dock->hide();
    return *Dock;
}

AIDebuggerPanel* AIDebuggerPanel::Find() { return Instance(); }

void AIDebuggerPanel::Show()
{
    AIDebuggerPanel* Dock = Find();
    if(!Dock) { return; }
    Dock->show();
    Dock->raise();
    Dock->Refresh();
}

AIDebuggerPanel::AIDebuggerPanel(QWidget* InParent) : QDockWidget(QStringLiteral("AI Debugger"), InParent)
{
    setObjectName(QStringLiteral("AIDebuggerDock"));
    EditorAI::InstallOverlay();
    EditorAI::OverlaySettings& Overlay = EditorAI::GetOverlaySettings();

    auto* Body = new QWidget(this);
    auto* Layout = new QVBoxLayout(Body);
    Header = new QLabel(Body);
    Header->setWordWrap(true);
    Layout->addWidget(Header);

    auto* Toggles = new QHBoxLayout();
    const auto Toggle = [&](const QString& InLabel, bool& InFlag)
    {
        auto* Box = new QCheckBox(InLabel, Body);
        Box->setChecked(InFlag);
        connect(Box, &QCheckBox::toggled, this, [&InFlag](const bool bInOn) { InFlag = bInOn; });
        Toggles->addWidget(Box);
        Synced.emplace_back(Box, &InFlag);
    };
    Toggle(QStringLiteral("Paths"), Overlay.bPaths);
    Toggle(QStringLiteral("Sight"), Overlay.bSight);
    Toggle(QStringLiteral("Navmesh"), Overlay.bNavMesh);
    Toggles->addStretch(1);
    Layout->addLayout(Toggles);

    auto* Split = new QSplitter(Qt::Vertical, Body);
    Agents = new QListWidget(Split);
    Details = new QTreeWidget(Split);
    Details->setColumnCount(2);
    Details->setHeaderLabels({ QStringLiteral("What"), QStringLiteral("Value") });
    Details->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Split->setStretchFactor(1, 3);
    Layout->addWidget(Split, 1);
    setWidget(Body);

    connect(Agents, &QListWidget::currentRowChanged, this, [this](const int InRow)
    {
        QListWidgetItem* Item = InRow >= 0 ? Agents->item(InRow) : nullptr;
        EditorAI::GetDebuggedAgent() = Item ? GameCore::EntityID::Unpack(Item->data(Qt::UserRole).toULongLong()) : GameCore::EntityID();
        if(Item && EditorAI::GetHost().Document) { EditorAI::GetHost().Document->SetSelection(EditorAI::GetDebuggedAgent()); }
        FillAgent();
    });
    /* Double-click: its tree, in the behavior tree editor, debugging it. */
    Agents->setToolTip(QStringLiteral("Double-click to open the agent's tree, its running branch lit"));
    connect(Agents, &QListWidget::itemDoubleClicked, this, [this]()
    {
        AICore::AIWorld* AI = EditorAI::IsPlaying() ? EditorAI::GetAI() : nullptr;
        const GameCore::EntityID Agent = EditorAI::GetDebuggedAgent();
        if(!AI || !AI->GetWorld().IsAlive(Agent)) { return; }
        const AICore::AIBrainComponent* Brain = AI->GetWorld().GetEntity(Agent).GetComponent<AICore::AIBrainComponent>();
        const QString File = Brain ? EditorContent::FileForRuntimePath(QString::fromUtf8(Brain->Tree)) : QString();
        if(File.isEmpty()) { Header->setText(QStringLiteral("Its tree has no source file in the project")); return; }
        if(BehaviorTreeEditorWindow* Window = BehaviorTreeEditorWindow::Open(File, window())) { Window->SetDebugging(true); }
    });
    Timer = new QTimer(this);
    Timer->setInterval(250);
    connect(Timer, &QTimer::timeout, this, [this]() { if(isVisible()) { Refresh(); } });
    Timer->start();
    if(EditorDocument* Document = EditorAI::GetHost().Document)
    {
        connect(Document, &EditorDocument::WorldReset, this, [this]() { EditorAI::GetDebuggedAgent() = GameCore::EntityID(); Refresh(); });
    }
}

void AIDebuggerPanel::SyncToggles()
{
    for(const auto& Each : Synced)
    {
        const QSignalBlocker Quiet(Each.first);
        Each.first->setChecked(*Each.second);
    }
}

void AIDebuggerPanel::PickRow(const int InRow)
{
    FillAgents();
    if(InRow >= 0 && InRow < Agents->count()) { Agents->setCurrentRow(InRow); }
}

void AIDebuggerPanel::Refresh()
{
    SyncToggles();
    FillAgents();
    FillAgent();
}

void AIDebuggerPanel::FillAgents()
{
    AICore::AIWorld* AI = EditorAI::IsPlaying() ? EditorAI::GetAI() : nullptr;
    if(!AI)
    {
        Header->setText(EditorAI::IsPlaying() ? QStringLiteral("This Play has no AI here (a client follows its server's)")
                                              : QStringLiteral("Press Play: the agents of the running level show here"));
        Agents->clear();
        return;
    }
    std::vector<std::pair<uint64_t, QString>> Rows;
    AI->ForEachBrain([&](const GameCore::EntityID InEntity, AICore::BTInstance& InBrain)
    {
        Rows.emplace_back(InEntity.Pack(), QStringLiteral("%1  (%2)").arg(NameOf(AI->GetWorld(), InEntity), QString::fromStdString(InBrain.GetAsset().Name)));
    });
    std::sort(Rows.begin(), Rows.end(), [](const auto& InA, const auto& InB) { return InA.second < InB.second; });
    Header->setText(QStringLiteral("%1 running brain(s)").arg(Rows.size()));

    /* The list in place, so the selection survives the refresh. */
    const uint64_t Current = EditorAI::GetDebuggedAgent().Pack();
    if(Agents->count() != int(Rows.size()))
    {
        Agents->blockSignals(true);
        Agents->clear();
        for(const auto& Row : Rows)
        {
            auto* Item = new QListWidgetItem(Row.second, Agents);
            Item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(Row.first));
            if(Row.first == Current) { Agents->setCurrentItem(Item); }
        }
        Agents->blockSignals(false);
    }
    else
    {
        for(int Index = 0; Index < Agents->count(); ++Index)
        {
            Agents->item(Index)->setText(Rows[size_t(Index)].second);
            Agents->item(Index)->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(Rows[size_t(Index)].first));
        }
    }
}

void AIDebuggerPanel::FillAgent()
{
    Details->clear();
    AICore::AIWorld* AI = EditorAI::IsPlaying() ? EditorAI::GetAI() : nullptr;
    const GameCore::EntityID Agent = EditorAI::GetDebuggedAgent();
    AICore::BTInstance* Brain = AI ? AI->FindBrain(Agent) : nullptr;
    if(!Brain) { return; }
    GameCore::World& World = AI->GetWorld();
    const AICore::BehaviorTreeAsset& Tree = Brain->GetAsset();

    auto* Running = new QTreeWidgetItem(Details, { QStringLiteral("Running"), QString::fromStdString(Tree.Name) });
    std::vector<uint32_t> Path;
    Brain->GetActivePath(Path);
    for(const uint32_t Node : Path)
    {
        const AICore::BTNodeDef& Def = Tree.Nodes[Node];
        const double Seconds = Brain->GetTime() - Brain->GetNodeMemory(Node).StartTime;
        auto* Item = new QTreeWidgetItem(Running, { QString(int(Def.Depth) * 2, QLatin1Char(' ')) + QString::fromUtf8(Def.GetDisplayName()),
                                                    QStringLiteral("%1 s").arg(Seconds, 0, 'f', 1) });
        if(Def.Kind == AICore::EBTNodeKind::Composite && Def.Composite == AICore::EBTComposite::UtilitySelector)
        {
            for(const uint32_t Child : Def.Children)
            {
                const float Score = Brain->GetUtilityScore(Child);
                new QTreeWidgetItem(Item, { QStringLiteral("score: ") + QString::fromUtf8(Tree.Nodes[Child].GetDisplayName()),
                                            Score < 0.0f ? QStringLiteral("-") : QString::number(double(Score), 'f', 2) });
            }
        }
    }
    Running->setExpanded(true);
    for(int Index = 0; Index < Running->childCount(); ++Index) { Running->child(Index)->setExpanded(true); }

    auto* Board = new QTreeWidgetItem(Details, { QStringLiteral("Blackboard"), QString() });
    const AICore::Blackboard& Keys = Brain->GetBlackboard();
    for(size_t Index = 0; Index < Keys.GetKeyCount(); ++Index)
    {
        const AICore::BlackboardValue& Value = Keys.GetValue(int32_t(Index));
        QString Text = !Value.bSet ? QStringLiteral("(unset)")
                     : Value.Type == AICore::EBlackboardType::Entity ? NameOf(World, Value.EntityValue)
                     : QString::fromStdString(Value.ToString());
        new QTreeWidgetItem(Board, { QString::fromStdString(Keys.GetKey(int32_t(Index)).Name), Text });
    }
    Board->setExpanded(true);

    auto* Memory = new QTreeWidgetItem(Details, { QStringLiteral("Perception"), QString() });
    if(const std::vector<AICore::PerceivedStimulus>* Seen = AI->GetPerception().GetMemory(Agent))
    {
        for(const AICore::PerceivedStimulus& S : *Seen)
        {
            new QTreeWidgetItem(Memory, { NameOf(World, S.Entity), QStringLiteral("%1, %2 s ago, at %3")
                                         .arg(S.bSeen ? QStringLiteral("seen") : (S.bHeard ? QStringLiteral("heard") : QStringLiteral("remembered")))
                                         .arg(double(S.Age), 0, 'f', 1).arg(Vec(S.LastKnownPosition)) });
        }
    }
    Memory->setExpanded(true);

    auto* Nav = new QTreeWidgetItem(Details, { QStringLiteral("Navigation"), QString() });
    if(const AICore::NavAgentComponent* Agent0 = World.GetEntity(Agent).GetComponent<AICore::NavAgentComponent>())
    {
        const char* Statuses[] = { "idle", "moving", "arrived", "failed" };
        new QTreeWidgetItem(Nav, { QStringLiteral("Status"), QString::fromLatin1(Statuses[std::min<int>(Agent0->Status, 3)]) });
        new QTreeWidgetItem(Nav, { QStringLiteral("Speed"), QStringLiteral("%1 cm/s").arg(double(Agent0->DesiredVelocity.GetLength()), 0, 'f', 0) });
        if(Agent0->bHasTarget)
        {
            new QTreeWidgetItem(Nav, { QStringLiteral("Target"), Vec(Agent0->Target) });
            new QTreeWidgetItem(Nav, { QStringLiteral("Remaining"), QStringLiteral("%1 m").arg(double(Agent0->RemainingDistance) / 100.0, 0, 'f', 1) });
        }
        std::vector<EngineCore::Vector3> Corners;
        new QTreeWidgetItem(Nav, { QStringLiteral("Corners"), QString::number(AI->GetNav().GetAgentCorners(Agent, Corners)) });
    }
    Nav->setExpanded(true);
}

#endif // WITH_EDITOR && WITH_AICORE
