#if WITH_EDITOR

#include "Private/Editor/Graph/GraphEditorWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>

#include "Public/NodeGraph/Schema.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorTab.h"

namespace
{
    /* The one window; a QPointer, so a closed (deleted) one reads as null. */
    QPointer<GraphEditorWindow>& Instance()
    {
        static QPointer<GraphEditorWindow> Window;
        return Window;
    }
}

GraphEditorWindow& GraphEditorWindow::Get()
{
    /* Top-level and unowned: the graph window may sit behind the main one. */
    if(!Instance()) { Instance() = new GraphEditorWindow(); }
    return *Instance();
}

GraphEditorWindow* GraphEditorWindow::Find()
{
    return Instance();
}

GraphEditorWindow::GraphEditorWindow(QWidget* InParent)
    : QMainWindow(InParent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 850);

    Tabs = new QTabWidget(this);
    Tabs->setObjectName(QStringLiteral("GraphTabs"));
    Tabs->setDocumentMode(true);
    Tabs->setTabsClosable(true);
    Tabs->setMovable(true);
    Tabs->tabBar()->setExpanding(false);
    Tabs->tabBar()->installEventFilter(this);
    setCentralWidget(Tabs);

    connect(Tabs, &QTabWidget::currentChanged, this, &GraphEditorWindow::UpdateTitles);
    connect(Tabs, &QTabWidget::tabCloseRequested, this, [this](const int InIndex)
    {
        if(auto* Tab = qobject_cast<GraphEditorTab*>(Tabs->widget(InIndex))) { CloseTab(Tab); }
    });

    const auto Step = [this](const QList<QKeySequence>& InKeys, const int InBy)
    {
        auto* Action = new QAction(this);
        Action->setShortcuts(InKeys);
        connect(Action, &QAction::triggered, this, [this, InBy]() { StepTab(InBy); });
        addAction(Action);
    };
    Step({ QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown) }, 1);
    Step({ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp) }, -1);

    UpdateTitles();
}

void GraphEditorWindow::AddTab(GraphEditorTab* InTab)
{
    Tabs->addTab(InTab, InTab->GetTabTitle());
    connect(InTab, &GraphEditorTab::TitleChanged, this, &GraphEditorWindow::UpdateTitles);
    connect(InTab, &GraphEditorTab::CloseRequested, this, [this, InTab]() { CloseTab(InTab); });
    ShowTab(InTab);
}

void GraphEditorWindow::ShowTab(GraphEditorTab* InTab)
{
    Tabs->setCurrentWidget(InTab);
    if(isMinimized()) { showNormal(); }
    else              { show(); }
    raise();
    activateWindow();
}

bool GraphEditorWindow::CloseTab(GraphEditorTab* InTab)
{
    const int Index = Tabs->indexOf(InTab);
    if(Index < 0 || !InTab->ConfirmClose()) { return false; }

    Tabs->removeTab(Index);
    InTab->hide();
    InTab->deleteLater();
    if(Tabs->count() == 0) { close(); }
    return true;
}

QList<GraphEditorTab*> GraphEditorWindow::GetTabs() const
{
    QList<GraphEditorTab*> Result;
    for(int Index = 0; Index < Tabs->count(); ++Index)
    {
        if(auto* Tab = qobject_cast<GraphEditorTab*>(Tabs->widget(Index))) { Result << Tab; }
    }
    return Result;
}

GraphEditorTab* GraphEditorWindow::CurrentTab() const
{
    return qobject_cast<GraphEditorTab*>(Tabs->currentWidget());
}

void GraphEditorWindow::UpdateTitles()
{
    for(GraphEditorTab* Tab : GetTabs())
    {
        const int Index = Tabs->indexOf(Tab);
        Tabs->setTabText(Index, Tab->GetTabTitle());
        Tabs->setTabToolTip(Index, Tab->GetDocument().GetPath());
    }

    const GraphEditorTab* Current = CurrentTab();
    setWindowTitle(Current ? QStringLiteral("%1 - %2 Graph - Nimble Editor")
                                 .arg(Current->GetTabTitle(), EditorGraphs::ToQ(Current->GetDocument().GetSchema().GetName()))
                           : QStringLiteral("Graph Editor - Nimble Editor"));
}

void GraphEditorWindow::StepTab(const int InBy)
{
    const int Count = Tabs->count();
    if(Count > 1) { Tabs->setCurrentIndex((Tabs->currentIndex() + InBy + Count) % Count); }
}

void GraphEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Every graph is asked before any goes, so a Cancel loses nothing. */
    for(GraphEditorTab* Tab : GetTabs())
    {
        if(!Tab->ConfirmClose())
        {
            InEvent->ignore();
            return;
        }
    }
    InEvent->accept();
}

bool GraphEditorWindow::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* A middle click on a tab closes it, as in a browser. */
    if(InWatched == Tabs->tabBar() && InEvent->type() == QEvent::MouseButtonRelease)
    {
        const auto* Mouse = static_cast<QMouseEvent*>(InEvent);
        if(Mouse->button() == Qt::MiddleButton)
        {
            const int Index = Tabs->tabBar()->tabAt(Mouse->position().toPoint());
            if(auto* Tab = qobject_cast<GraphEditorTab*>(Tabs->widget(Index))) { CloseTab(Tab); }
            return true;
        }
    }
    return QMainWindow::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
