#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>

#include "Private/Editor/Sequencer/SequencerTab.h"

namespace
{
    /* The one window; a QPointer, so a closed (deleted) one reads as null. */
    QPointer<SequencerWindow>& Instance()
    {
        static QPointer<SequencerWindow> Window;
        return Window;
    }
}

SequencerWindow& SequencerWindow::Get()
{
    /* Top-level and unowned: the Sequencer may sit beside the main window, whose viewport it drives. */
    if(!Instance()) { Instance() = new SequencerWindow(); }
    return *Instance();
}

SequencerWindow* SequencerWindow::Find()
{
    return Instance();
}

SequencerWindow::SequencerWindow(QWidget* InParent)
    : QMainWindow(InParent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1500, 700);

    Tabs = new QTabWidget(this);
    Tabs->setObjectName(QStringLiteral("SequencerTabs"));
    Tabs->setDocumentMode(true);
    Tabs->setTabsClosable(true);
    Tabs->setMovable(true);
    Tabs->tabBar()->setExpanding(false);
    Tabs->tabBar()->installEventFilter(this);
    setCentralWidget(Tabs);

    connect(Tabs, &QTabWidget::currentChanged, this, &SequencerWindow::UpdateTitles);
    connect(Tabs, &QTabWidget::tabCloseRequested, this, [this](const int InIndex)
    {
        if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(InIndex))) { CloseTab(Tab); }
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

void SequencerWindow::AddTab(SequencerTab* InTab)
{
    Tabs->addTab(InTab, InTab->GetTabTitle());
    connect(InTab, &SequencerTab::TitleChanged, this, &SequencerWindow::UpdateTitles);
    connect(InTab, &SequencerTab::CloseRequested, this, [this, InTab]() { CloseTab(InTab); });
    ShowTab(InTab);
}

void SequencerWindow::ShowTab(SequencerTab* InTab)
{
    Tabs->setCurrentWidget(InTab);
    if(isMinimized()) { showNormal(); }
    else              { show(); }
    raise();
    activateWindow();
}

bool SequencerWindow::CloseTab(SequencerTab* InTab)
{
    const int Index = Tabs->indexOf(InTab);
    if(Index < 0 || !InTab->ConfirmClose()) { return false; }

    /* Now, not later: the tab puts the level back as it goes. */
    Tabs->removeTab(Index);
    delete InTab;
    if(Tabs->count() == 0) { close(); }
    return true;
}

QList<SequencerTab*> SequencerWindow::GetTabs() const
{
    QList<SequencerTab*> Result;
    for(int Index = 0; Index < Tabs->count(); ++Index)
    {
        if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(Index))) { Result << Tab; }
    }
    return Result;
}

SequencerTab* SequencerWindow::CurrentTab() const
{
    return qobject_cast<SequencerTab*>(Tabs->currentWidget());
}

void SequencerWindow::UpdateTitles()
{
    for(SequencerTab* Tab : GetTabs())
    {
        const int Index = Tabs->indexOf(Tab);
        Tabs->setTabText(Index, Tab->GetTabTitle());
        Tabs->setTabToolTip(Index, Tab->GetPath());
    }

    const SequencerTab* Current = CurrentTab();
    setWindowTitle(Current ? QStringLiteral("%1 - Sequencer - Nimble Editor").arg(Current->GetTabTitle())
                           : QStringLiteral("Sequencer - Nimble Editor"));
}

void SequencerWindow::StepTab(const int InBy)
{
    const int Count = Tabs->count();
    if(Count > 1) { Tabs->setCurrentIndex((Tabs->currentIndex() + InBy + Count) % Count); }
}

void SequencerWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Every sequence is asked before any goes, so a Cancel loses nothing. */
    for(SequencerTab* Tab : GetTabs())
    {
        if(!Tab->ConfirmClose())
        {
            InEvent->ignore();
            return;
        }
    }

    /* The tabs go now, each putting the level back. */
    for(SequencerTab* Tab : GetTabs())
    {
        Tabs->removeTab(Tabs->indexOf(Tab));
        delete Tab;
    }
    InEvent->accept();
}

bool SequencerWindow::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* A middle click on a tab closes it, as in a browser. */
    if(InWatched == Tabs->tabBar() && InEvent->type() == QEvent::MouseButtonRelease)
    {
        const auto* Mouse = static_cast<QMouseEvent*>(InEvent);
        if(Mouse->button() == Qt::MiddleButton)
        {
            const int Index = Tabs->tabBar()->tabAt(Mouse->position().toPoint());
            if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(Index))) { CloseTab(Tab); }
            return true;
        }
    }
    return QMainWindow::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
