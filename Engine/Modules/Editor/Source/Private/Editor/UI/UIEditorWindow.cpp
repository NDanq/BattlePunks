#if WITH_EDITOR

#include "Private/Editor/UI/UIEditorWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>

#include "Private/Editor/UI/UIEditorTab.h"

namespace
{
    /* The one window; a QPointer, so a closed (deleted) one reads as null. */
    QPointer<UIEditorWindow>& Instance()
    {
        static QPointer<UIEditorWindow> Window;
        return Window;
    }
}

UIEditorWindow& UIEditorWindow::Get()
{
    /* Top-level and unowned: the designer may sit behind the main one. */
    if(!Instance()) { Instance() = new UIEditorWindow(); }
    return *Instance();
}

UIEditorWindow* UIEditorWindow::Find()
{
    return Instance();
}

UIEditorWindow::UIEditorWindow(QWidget* InParent)
    : QMainWindow(InParent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 850);

    Tabs = new QTabWidget(this);
    Tabs->setObjectName(QStringLiteral("UITabs"));
    Tabs->setDocumentMode(true);
    Tabs->setTabsClosable(true);
    Tabs->setMovable(true);
    Tabs->tabBar()->setExpanding(false);
    Tabs->tabBar()->installEventFilter(this);
    setCentralWidget(Tabs);

    connect(Tabs, &QTabWidget::currentChanged, this, &UIEditorWindow::UpdateTitles);
    connect(Tabs, &QTabWidget::tabCloseRequested, this, [this](const int InIndex)
    {
        if(auto* Tab = qobject_cast<UIEditorTab*>(Tabs->widget(InIndex))) { CloseTab(Tab); }
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

void UIEditorWindow::AddTab(UIEditorTab* InTab)
{
    Tabs->addTab(InTab, InTab->GetTabTitle());
    connect(InTab, &UIEditorTab::TitleChanged, this, &UIEditorWindow::UpdateTitles);
    connect(InTab, &UIEditorTab::CloseRequested, this, [this, InTab]() { CloseTab(InTab); });
    ShowTab(InTab);
}

void UIEditorWindow::ShowTab(UIEditorTab* InTab)
{
    Tabs->setCurrentWidget(InTab);
    if(isMinimized()) { showNormal(); }
    else              { show(); }
    raise();
    activateWindow();
}

bool UIEditorWindow::CloseTab(UIEditorTab* InTab)
{
    const int Index = Tabs->indexOf(InTab);
    if(Index < 0 || !InTab->ConfirmClose()) { return false; }

    /* Now, not later: the tab's viewport releases its swapchain against its
     * native window, which must still exist. */
    Tabs->removeTab(Index);
    delete InTab;
    if(Tabs->count() == 0) { close(); }
    return true;
}

QList<UIEditorTab*> UIEditorWindow::GetTabs() const
{
    QList<UIEditorTab*> Result;
    for(int Index = 0; Index < Tabs->count(); ++Index)
    {
        if(auto* Tab = qobject_cast<UIEditorTab*>(Tabs->widget(Index))) { Result << Tab; }
    }
    return Result;
}

UIEditorTab* UIEditorWindow::CurrentTab() const
{
    return qobject_cast<UIEditorTab*>(Tabs->currentWidget());
}

void UIEditorWindow::UpdateTitles()
{
    for(UIEditorTab* Tab : GetTabs())
    {
        const int Index = Tabs->indexOf(Tab);
        Tabs->setTabText(Index, Tab->GetTabTitle());
        Tabs->setTabToolTip(Index, Tab->GetPath());
    }

    const UIEditorTab* Current = CurrentTab();
    setWindowTitle(Current ? QStringLiteral("%1 - UI Designer - Nimble Editor").arg(Current->GetTabTitle())
                           : QStringLiteral("UI Designer - Nimble Editor"));
}

void UIEditorWindow::StepTab(const int InBy)
{
    const int Count = Tabs->count();
    if(Count > 1) { Tabs->setCurrentIndex((Tabs->currentIndex() + InBy + Count) % Count); }
}

void UIEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Every document is asked before any goes, so a Cancel loses nothing. */
    for(UIEditorTab* Tab : GetTabs())
    {
        if(!Tab->ConfirmClose())
        {
            InEvent->ignore();
            return;
        }
    }

    /* The tabs go now, while their viewports' native windows still exist. */
    for(UIEditorTab* Tab : GetTabs())
    {
        Tabs->removeTab(Tabs->indexOf(Tab));
        delete Tab;
    }
    InEvent->accept();
}

bool UIEditorWindow::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* A middle click on a tab closes it, as in a browser. */
    if(InWatched == Tabs->tabBar() && InEvent->type() == QEvent::MouseButtonRelease)
    {
        const auto* Mouse = static_cast<QMouseEvent*>(InEvent);
        if(Mouse->button() == Qt::MiddleButton)
        {
            const int Index = Tabs->tabBar()->tabAt(Mouse->position().toPoint());
            if(auto* Tab = qobject_cast<UIEditorTab*>(Tabs->widget(Index))) { CloseTab(Tab); }
            return true;
        }
    }
    return QMainWindow::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
